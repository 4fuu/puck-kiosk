#include "app/car_ui.h"
#include "app/diagnostics.h"
#include "app/media_overlay.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
#include "app/ui_focus.h"
#include "app/ui_input.h"
#include "platform/audio.h"
#include "platform/http.h"
#include "puck/puck_view.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, const char* file, int line,
           const std::string& detail = {}) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: check failed: %s", file, line, expression);
    if (!detail.empty()) {
        std::fprintf(stderr, " (%s)", detail.c_str());
    }
    std::fputc('\n', stderr);
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define CHECK_MSG(expression, detail) check((expression), #expression, __FILE__, __LINE__, (detail))

class FakeCanvas final : public pk::Canvas {
public:
    bool initialize(const std::string&, std::string*) override { return true; }
    void begin_frame(pk::Size, float) override {}
    void end_frame() override {}
    pk::ImageHandle create_image(const std::uint8_t*, int, int) override { return 0; }
    void draw_image(pk::ImageHandle, pk::Rect, float) override {}
    void release_image(pk::ImageHandle) override {}
    void clear(pk::Color) override {}
    void fill_rect(pk::Rect, pk::Color) override {}
    void fill_rounded_rect(pk::Rect, float, pk::Color) override {}
    void fill_circle(float, float, float, pk::Color) override {}
    void fill_ellipse(float, float, float, float, pk::Color) override {}
    void fill_triangle(float, float, float, float, float, float, pk::Color) override {}
    void stroke_circle(float, float, float, float, pk::Color) override {}
    void stroke_line(float, float, float, float, float, pk::Color) override {}
    void text(float, float, const pk::TextStyle&, const std::string& value) override {
        texts.push_back(value);
    }
    float measure_text(const pk::TextStyle& style, const std::string& value) override {
        return static_cast<float>(value.size()) * style.size * 0.5f;
    }
    void fill_path(const std::string&, pk::Rect, pk::Rect, pk::Color) override {}

    std::vector<std::string> texts;
};

class FakePuck final : public pk::PuckView {
public:
    const char* name() const override { return "fake"; }
    bool is_3d() const override { return false; }
    bool initialize(std::string*) override { return true; }
    void draw(pk::Canvas&, pk::Rect, const pk::PuckVisual& visual, float) override {
        last_visual = visual;
        ++draws;
    }

    pk::PuckVisual last_visual;
    int draws = 0;
};

class FakeHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& value, std::string* error) override {
        requests.push_back(value);
        if (!error_message.empty() && error) {
            *error = error_message;
        }
        pk::HttpResponse response;
        response.status = status;
        return response;
    }

    std::vector<pk::HttpRequest> requests;
    int status = 200;
    std::string error_message;
};

class FakeAudio final : public pk::AudioIO {
public:
    const char* name() const override { return "fake-audio"; }
    bool start_capture(int, int, std::string*) override {
        running = true;
        phase = 0.0;
        return true;
    }
    void stop_capture() override { running = false; }
    std::size_t read_capture(std::int16_t* destination, std::size_t frames) override {
        if (!running || !destination) {
            return 0;
        }
        for (std::size_t i = 0; i < frames; ++i) {
            destination[i] = static_cast<std::int16_t>(std::sin(phase) * 12000.0);
            phase += 2.0 * 3.141592653589793 * 440.0 / 48000.0;
        }
        return frames;
    }
    bool start_playback(int, int, std::string*) override { return true; }
    void stop_playback() override {}
    std::size_t write_playback(const std::int16_t*, std::size_t frames) override { return frames; }

    bool running = false;
    double phase = 0.0;
};

pk::InputEvent pointer(pk::InputEvent::Type type, float x, float y) {
    pk::InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

pk::InputEvent gamepad_button(pk::InputEvent::GamepadButton button, bool down) {
    pk::InputEvent event;
    event.type = down ? pk::InputEvent::Type::GamepadButtonDown
                      : pk::InputEvent::Type::GamepadButtonUp;
    event.button = button;
    return event;
}

pk::InputEvent gamepad_axis(pk::InputEvent::GamepadAxis axis, float value) {
    pk::InputEvent event;
    event.type = pk::InputEvent::Type::GamepadAxis;
    event.axis = axis;
    event.value = value;
    return event;
}

const pk::UiTarget* find_target(const pk::UiHitMap& hits, pk::UiAction action) {
    for (const pk::UiTarget& target : hits.targets()) {
        if (target.action == action) {
            return &target;
        }
    }
    return nullptr;
}

int count_targets(const pk::UiHitMap& hits, pk::UiAction action) {
    int count = 0;
    for (const pk::UiTarget& target : hits.targets()) {
        if (target.action == action) {
            ++count;
        }
    }
    return count;
}

const pk::UiTarget* find_indexed_target(const pk::UiHitMap& hits, pk::UiAction action,
                                        int index) {
    for (const pk::UiTarget& target : hits.targets()) {
        if (target.action == action && target.index == index) {
            return &target;
        }
    }
    return nullptr;
}

bool has_text(const FakeCanvas& canvas, const std::string& value) {
    for (const std::string& text : canvas.texts) {
        if (text == value) {
            return true;
        }
    }
    return false;
}

bool has_text_containing(const FakeCanvas& canvas, const std::string& value) {
    for (const std::string& text : canvas.texts) {
        if (text.find(value) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool overlaps(pk::Rect a, pk::Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void test_hit_map() {
    pk::UiHitMap hits;
    hits.add(pk::Rect{0, 0, 100, 100}, pk::UiAction::Mute);
    hits.add(pk::Rect{50, 50, 100, 100}, pk::UiAction::End);
    CHECK(hits.hit(10, 10) != nullptr);
    CHECK(hits.hit(10, 10)->action == pk::UiAction::Mute);
    CHECK(hits.hit(60, 60) != nullptr);
    CHECK(hits.hit(60, 60)->action == pk::UiAction::End);
    CHECK(hits.hit(200, 200) == nullptr);
    CHECK(hits.hit(100, 100) != nullptr);
    CHECK(hits.hit(100, 100)->action == pk::UiAction::End);
    CHECK(hits.hit(150, 150)->action == pk::UiAction::End);

    hits.add(pk::Rect{0, 0, 200, 200}, pk::UiAction::None);
    CHECK(hits.hit(60, 60)->action == pk::UiAction::None);
    CHECK(hits.targets().size() == 3);

    pk::UiHitMap empty;
    empty.add(pk::Rect{0, 0, 0, 10}, pk::UiAction::Mute);
    CHECK(empty.empty());
    CHECK(empty.hit(0, 0) == nullptr);
}

void test_router() {
    pk::UiHitMap hits;
    hits.add(pk::Rect{100, 100, 80, 60}, pk::UiAction::Mute, 3);
    pk::UiInputRouter router;
    router.set_hit_map(&hits);

    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerDown, 140, 130)) ==
          pk::UiAction::None);
    CHECK(router.pressed());
    int index = -1;
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, 140, 130), &index) ==
          pk::UiAction::Mute);
    CHECK(index == 3);
    CHECK(!router.pressed());

    // Releasing outside the pressed rect cancels.
    router.handle(pointer(pk::InputEvent::Type::PointerDown, 140, 130));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, 300, 300)) ==
          pk::UiAction::None);

    // Pressing empty space cannot fire on release over a button.
    router.handle(pointer(pk::InputEvent::Type::PointerDown, 10, 10));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, 140, 130)) ==
          pk::UiAction::None);

    pk::InputEvent escape;
    escape.type = pk::InputEvent::Type::KeyDown;
    escape.key = 27;
    CHECK(router.handle(escape) == pk::UiAction::Escape);
    escape.key = 1;
    CHECK(router.handle(escape) == pk::UiAction::Escape);
    escape.key = 2;
    CHECK(router.handle(escape) == pk::UiAction::None);

    pk::InputEvent quit;
    quit.type = pk::InputEvent::Type::Quit;
    CHECK(router.handle(quit) == pk::UiAction::None);
    CHECK(router.quit_requested());
}

void test_script() {
    const std::string text = R"([
        {"t": 0.5, "type": "up", "x": 10, "y": 20},
        {"t": 0.1, "type": "down", "x": 30, "y": 40},
        {"t": 0.2, "type": "key", "key": "Escape"},
        {"t": 0.3, "type": "quit"}
    ])";
    std::vector<pk::ScriptedInput> events;
    std::string error;
    CHECK_MSG(pk::parse_input_script(text, &events, &error), error);
    CHECK(events.size() == 4);
    CHECK(events[0].event.type == pk::InputEvent::Type::PointerDown);
    CHECK(events[0].t == 0.1);
    CHECK(events[1].event.type == pk::InputEvent::Type::KeyDown);
    CHECK(events[1].event.key == 27);
    CHECK(events[2].event.type == pk::InputEvent::Type::Quit);
    CHECK(events[3].event.type == pk::InputEvent::Type::PointerUp);
    CHECK(events[3].event.x == 10.0f);

    std::vector<pk::ScriptedInput> bad;
    CHECK(!pk::parse_input_script("{}", &bad, &error));
    CHECK(!pk::parse_input_script("[{\"type\":\"nope\"}]", &bad, &error));
    CHECK(!pk::parse_input_script("[{\"type\":\"down\",\"key\":\"A\"}]", &bad, &error));
    CHECK(!pk::parse_input_script("not json", &bad, &error));
}

void test_dispatch() {
    int call = 0;
    int mute = 0;
    int end = 0;
    int media_close = 0;
    int option = -1;
    int echo = -1;
    int passkey = 0;
    int pairing_retry = 0;
    int window_minimize = 0;
    int window_maximize = 0;
    int window_close = 0;
    pk::UiActionHandlers handlers;
    handlers.call = [&]() { ++call; };
    handlers.mute = [&]() { ++mute; };
    handlers.end = [&]() { ++end; };
    handlers.media_close = [&]() { ++media_close; };
    handlers.sheet_option = [&](int index) { option = index; };
    handlers.echo_mode = [&](int index) { echo = index; };
    handlers.passkey_confirm = [&]() { ++passkey; };
    handlers.pairing_retry = [&]() { ++pairing_retry; };
    handlers.window_minimize = [&]() { ++window_minimize; };
    handlers.window_maximize = [&]() { ++window_maximize; };
    handlers.window_close = [&]() { ++window_close; };

    CHECK(pk::dispatch_ui_action(pk::UiAction::Call, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::Mute, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::End, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::MediaClose, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::SheetOption, 2, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::EchoMode, 1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::PasskeyConfirm, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::PairingRetry, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::WindowMinimize, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::WindowMaximize, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::WindowClose, -1, handlers));
    CHECK(call == 1 && mute == 1 && end == 1 && media_close == 1);
    CHECK(option == 2 && echo == 1 && passkey == 1 && pairing_retry == 1);
    CHECK(window_minimize == 1 && window_maximize == 1 && window_close == 1);
    CHECK(pk::dispatch_ui_action(pk::UiAction::SheetScrollUp, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::SheetScrollDown, -1, handlers));
    CHECK(!pk::dispatch_ui_action(pk::UiAction::None, -1, handlers));
    CHECK(!pk::dispatch_ui_action(pk::UiAction::Escape, -1, handlers));
    CHECK(!pk::dispatch_ui_action(pk::UiAction::MicTest, -1, handlers));
}

nlohmann::json plugin_event(const std::string& name, nlohmann::json data) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", "plugin_message"},
        {"params",
         {{"type", "event"}, {"event", name}, {"data", std::move(data)}}},
    };
}

nlohmann::json request_json(const std::string& id, const std::string& method,
                            nlohmann::json options, const std::string& status = "pending") {
    return nlohmann::json{
        {"requestId", id},
        {"method", method},
        {"params", {{"options", std::move(options)}}},
        {"status", status},
    };
}

void test_plugin_ui() {
    pk::PluginUi plugins;
    std::vector<std::string> sent;
    plugins.set_sender([&](const std::string& text) {
        sent.push_back(text);
        return true;
    });

    nlohmann::json requests = nlohmann::json::array({
        request_json("r1", "ui.select",
                     {{"title", "Pick a track"},
                      {"message", "Choose one"},
                      {"options", {"Midnight", "Neon", "Quiet"}}}),
        request_json("r2", "ui.confirm", {{"title", "Delete?"}, {"confirmButtonText", "Delete"}}),
        request_json("r3", "ui.confirm", {{"title", "Approve on phone"}, {"fields", nlohmann::json::array()}}),
        request_json("r4", "ui.input", {{"title", "Your name"}}),
    });
    plugins.on_actor_message(plugin_event("plugin.ui.snapshot", {{"requests", requests}}).dump());

    pk::PluginSheet sheet = plugins.sheet();
    CHECK(sheet.kind == pk::PluginSheetKind::Select);
    CHECK(sheet.request_id == "r1");
    CHECK(sheet.title == "Pick a track");
    CHECK(sheet.message == "Choose one");
    CHECK(sheet.options.size() == 3);
    CHECK(plugins.pending_count() == 4);

    CHECK(plugins.select(1));
    CHECK(sent.size() == 1);
    nlohmann::json response = nlohmann::json::parse(sent[0], nullptr, false);
    CHECK(!response.is_discarded());
    CHECK(response["method"] == "executor_plugin_message");
    const nlohmann::json& message = response["params"]["message"];
    CHECK(message["type"] == "request");
    CHECK(message["id"] == "plugin-ui-respond-r1-1");
    CHECK(message["method"] == "plugin.ui.respond");
    CHECK(message["params"]["requestId"] == "r1");
    CHECK(message["params"]["result"] == "Neon");
    CHECK(!plugins.select(0));  // the select is resolved

    sheet = plugins.sheet();
    CHECK(sheet.kind == pk::PluginSheetKind::Confirm);
    CHECK(sheet.request_id == "r2");
    CHECK(sheet.confirm_label == "Delete");
    CHECK(plugins.confirm(false));
    CHECK(sent.size() == 2);
    response = nlohmann::json::parse(sent[1], nullptr, false);
    CHECK(response["params"]["message"]["params"]["result"] == false);
    CHECK(response["params"]["message"]["id"] == "plugin-ui-respond-r2-2");

    sheet = plugins.sheet();
    CHECK(sheet.kind == pk::PluginSheetKind::Phone);
    CHECK(sheet.title == "Approve on phone");
    CHECK(plugins.dismiss());
    CHECK(sent.size() == 2);  // phone OK only hides locally
    sheet = plugins.sheet();
    CHECK(sheet.kind == pk::PluginSheetKind::Phone);
    CHECK(sheet.title == "Your name");
    plugins.phone_ok();
    CHECK(plugins.sheet().kind == pk::PluginSheetKind::None);

    // A later snapshot must not resurrect a locally answered or hidden
    // request, even when the server's snapshot still says "pending".
    plugins.on_actor_message(plugin_event("plugin.ui.snapshot", {{"requests", requests}}).dump());
    CHECK(plugins.sheet().kind == pk::PluginSheetKind::None);

    // Settled requests do not show.
    pk::PluginUi settled;
    settled.set_sender([&](const std::string&) { return true; });
    settled.on_actor_message(plugin_event(
        "plugin.ui.snapshot",
        {{"requests", nlohmann::json::array(
                          {request_json("s1", "ui.select",
                                        {{"title", "Later"}, {"options", {"A", "B"}}},
                                        "responded")})}})
                                 .dump());
    CHECK(settled.sheet().kind == pk::PluginSheetKind::None);

    // request_added + request_removed.
    pk::PluginUi added;
    added.set_sender([&](const std::string&) { return true; });
    added.on_actor_message(plugin_event(
        "plugin.ui.request_added",
        {{"request", request_json("a1", "ui.select", {{"title", "Added"}, {"options", {"X", "Y"}}})}})
        .dump());
    CHECK(added.sheet().kind == pk::PluginSheetKind::Select);
    CHECK(added.sheet().title == "Added");
    added.on_actor_message(plugin_event("plugin.ui.request_removed", {{"requestId", "a1"}}).dump());
    CHECK(added.sheet().kind == pk::PluginSheetKind::None);

    // Dismissing a select answers with no result.
    pk::PluginUi cancel;
    std::vector<std::string> cancel_sent;
    cancel.set_sender([&](const std::string& text) {
        cancel_sent.push_back(text);
        return true;
    });
    cancel.on_actor_message(plugin_event(
        "plugin.ui.snapshot",
        {{"requests", nlohmann::json::array(
                          {request_json("c1", "ui.select", {{"title", "Cancel"}, {"options", {"A"}}})})}})
        .dump());
    CHECK(cancel.dismiss());
    CHECK(cancel_sent.size() == 1);
    const nlohmann::json cancel_response = nlohmann::json::parse(cancel_sent[0], nullptr, false);
    CHECK(cancel_response["params"]["message"]["params"]["requestId"] == "c1");
    CHECK(!cancel_response["params"]["message"]["params"].contains("result"));
    CHECK(cancel.sheet().kind == pk::PluginSheetKind::None);
}

void test_passkey() {
    pk::PasskeyFlow flow;
    CHECK(flow.mode() == pk::PasskeyMode::Hidden);

    flow.set_context(pk::PasskeyContext{true, true, false, false});
    CHECK(flow.mode() == pk::PasskeyMode::Phone);
    pk::PasskeyView view = flow.view();
    CHECK(view.title == "Confirm Passkey on Your Phone");
    CHECK(view.steps.size() == 3);
    CHECK(view.steps[0].title == "Open your phone\u2019s browser");
    CHECK(!view.confirm_button);
    CHECK(!flow.confirm());

    flow.set_context(pk::PasskeyContext{true, true, true, true});
    CHECK(flow.mode() == pk::PasskeyMode::Hidden);

    flow.set_context(pk::PasskeyContext{false, true, false, true});
    CHECK(flow.mode() == pk::PasskeyMode::Local);
    view = flow.view();
    CHECK(view.title == "Confirm Your Passkey");
    CHECK(view.steps.size() == 3);
    CHECK(view.confirm_button);
    CHECK(flow.confirm());
    CHECK(flow.mode() == pk::PasskeyMode::Confirming);
    CHECK(flow.view().confirming);
    CHECK(!flow.confirm());

    flow.on_request_result(false, "Not authorized");
    CHECK(flow.mode() == pk::PasskeyMode::Failed);
    CHECK(flow.view().status == "Not authorized");
    CHECK(flow.view().confirm_button);
    CHECK(flow.confirm());
    flow.on_request_result(true, {});
    CHECK(flow.mode() == pk::PasskeyMode::Confirming);
    flow.set_context(pk::PasskeyContext{false, true, true, true});
    CHECK(flow.mode() == pk::PasskeyMode::Hidden);
}

void test_diagnostics() {
    pk::EchoCancellationMode mode = pk::EchoCancellationMode::On;
    CHECK(pk::parse_echo_cancellation("remote-only", &mode));
    CHECK(mode == pk::EchoCancellationMode::RemoteOnly);
    CHECK(pk::parse_echo_cancellation("off", &mode));
    CHECK(mode == pk::EchoCancellationMode::Off);
    CHECK(!pk::parse_echo_cancellation("maybe", &mode));
    CHECK(std::string(pk::echo_cancellation_value(pk::EchoCancellationMode::RemoteOnly)) ==
          "remote-only");

    FakeHttp http;
    pk::Diagnostics diagnostics(&http, "https://ampcode.com");
    diagnostics.set_environment(pk::Size{1280, 720}, 1.0f, "headless", false);
    diagnostics.set_audio("fake-audio", true);
    diagnostics.set_echo_mode(pk::EchoCancellationMode::RemoteOnly);

    const nlohmann::json payload = diagnostics.payload();
    CHECK(payload["screen"] == "puck");
    CHECK(payload["userAgent"] == "puck-kiosk/0.1 (headless)");
    CHECK(payload["innerWidth"] == 1280);
    CHECK(payload["innerHeight"] == 720);
    CHECK(payload["devicePixelRatio"] == 1.0);
    CHECK(payload["maxTouchPoints"] == 0);
    CHECK(payload["pointer"] == "none");
    CHECK(payload["webRTC"] == true);
    CHECK(payload["audioContextSampleRate"] == 48000);
    CHECK(payload["microphonePermission"] == "granted");
    CHECK(payload["echoCancellation"] == "remote-only");
    CHECK(payload["microphone"].is_null());
    CHECK(payload["devices"][0]["kind"] == "audioinput");
    CHECK(payload["devices"][0]["label"] == "fake-audio");

    std::string error;
    CHECK(!diagnostics.send(&error));
    CHECK(diagnostics.send_status() == "Sign in to send");
    CHECK(http.requests.empty());

    diagnostics.set_session(true, "thread-1");
    CHECK(diagnostics.send(&error));
    CHECK(diagnostics.send_status() == "Sent");
    CHECK(http.requests.size() == 1);
    CHECK(http.requests[0].method == "POST");
    CHECK(http.requests[0].url == "https://ampcode.com/api/car/diagnostics");
    const nlohmann::json sent_body = nlohmann::json::parse(http.requests[0].body, nullptr, false);
    CHECK(sent_body["screen"] == "puck");
    CHECK(sent_body["echoCancellation"] == "remote-only");

    http.status = 500;
    CHECK(!diagnostics.send(&error));
    CHECK(diagnostics.send_status() == "Failed (500)");

    FakeAudio audio;
    CHECK(diagnostics.run_mic_test(&audio, &error));
    CHECK(diagnostics.payload()["microphone"]["result"] == "ok");
    CHECK(diagnostics.payload()["microphone"]["label"] == "fake-audio");
    CHECK(diagnostics.payload()["microphone"]["peakDBFS"].get<double>() > -12.0);
    CHECK(diagnostics.view().mic_status.find("OK, peak") == 0);

    diagnostics.set_call_active(true);
    CHECK(!diagnostics.run_mic_test(&audio, &error));
    CHECK(diagnostics.view().mic_status == "Microphone is busy in a call");
    CHECK(diagnostics.view().mic_busy);

    pk::voice::AecStatus aec_status;
    aec_status.initialized = true;
    aec_status.echo_cancellation = true;
    aec_status.noise_suppression = true;
    aec_status.backend = "speexdsp";
    aec_status.detail = "portable backend";
    diagnostics.set_call_aec_status(true, aec_status);
    CHECK(diagnostics.view().aec_current_known);
    CHECK(diagnostics.view().aec_current_active);
    CHECK(diagnostics.view().aec_current_backend == "speexdsp");
    CHECK(diagnostics.payload()["echoCancellationCurrent"]["backend"] == "speexdsp");
    diagnostics.set_call_aec_status(false, aec_status);
    CHECK(!diagnostics.view().aec_current_active);
    CHECK(!diagnostics.view().aec_current_in_call);
    CHECK(diagnostics.view().aec_current_detail == "not in a call");
    diagnostics.clear_call_aec_status();
    CHECK(!diagnostics.view().aec_current_known);

    FakeHttp usage_http;
    CHECK(pk::post_car_usage(usage_http, "https://ampcode.com", "direct", true, 1280, 720, &error));
    CHECK(usage_http.requests.size() == 1);
    CHECK(usage_http.requests[0].url == "https://ampcode.com/api/car/usage");
    const nlohmann::json usage =
        nlohmann::json::parse(usage_http.requests[0].body, nullptr, false);
    CHECK(usage["entry"] == "direct");
    CHECK(usage["coarsePointer"] == true);
    CHECK(usage["viewportWidth"] == 1280);
    CHECK(usage["viewportHeight"] == 720);
}

void test_car_ui_targets() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    pk::CallView view;
    view.phase = pk::CallPhase::Active;
    view.caption = "On a call with Puck";
    ui.set_call(view);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);

    // The Debug entry is hidden in normal use; F12 reveals it.
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugOpen) == nullptr);
    pk::InputEvent reveal;
    reveal.type = pk::InputEvent::Type::KeyDown;
    reveal.key = 123;
    ui.notify_input(reveal);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugOpen) != nullptr);
    // F12 again hides it; five quick corner taps reveal it for touch users.
    ui.notify_input(reveal);
    ui.draw(canvas, pk::Size{1280, 720}, 0.1f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugOpen) == nullptr);
    for (int i = 0; i < 5; ++i) {
        pk::InputEvent tap;
        tap.type = pk::InputEvent::Type::PointerDown;
        tap.x = 20.0f;
        tap.y = 700.0f;
        ui.notify_input(tap);
        tap.type = pk::InputEvent::Type::PointerUp;
        ui.notify_input(tap);
        ui.draw(canvas, pk::Size{1280, 720}, 0.2f + 0.1f * static_cast<float>(i));
    }
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugOpen) != nullptr);

    const pk::UiTarget* mute = find_target(ui.hit_map(), pk::UiAction::Mute);
    const pk::UiTarget* end = find_target(ui.hit_map(), pk::UiAction::End);
    const pk::UiTarget* debug = find_target(ui.hit_map(), pk::UiAction::DebugOpen);
    CHECK(mute != nullptr);
    CHECK(end != nullptr);
    CHECK(debug != nullptr);
    // The wide car layout puts End above Mute in the left control rail.
    CHECK(std::fabs(mute->rect.center_x() - end->rect.center_x()) < 0.5f);
    CHECK(end->rect.y < mute->rect.y);

    // Portrait keeps the web's horizontal pair, with Mute on the left.
    ui.draw(canvas, pk::Size{720, 1280}, 0.0f);
    mute = find_target(ui.hit_map(), pk::UiAction::Mute);
    end = find_target(ui.hit_map(), pk::UiAction::End);
    CHECK(mute != nullptr);
    CHECK(end != nullptr);
    CHECK(mute->rect.x < end->rect.x);

    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    router.handle(pointer(pk::InputEvent::Type::PointerDown, mute->rect.center_x(),
                          mute->rect.center_y()));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, mute->rect.center_x(),
                                mute->rect.center_y())) == pk::UiAction::Mute);

    // A select sheet covers the stage and owns its option rects.
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.title = "Pick a track";
    sheet.options = {"One", "Two", "Three", "Four"};
    ui.set_sheet(sheet);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(count_targets(ui.hit_map(), pk::UiAction::SheetOption) == 4);
    CHECK(find_target(ui.hit_map(), pk::UiAction::SheetDismiss) != nullptr);
    CHECK(find_target(ui.hit_map(), pk::UiAction::Mute) != nullptr);  // controls stay live

    // The passkey guide replaces the call controls with its confirm button.
    ui.set_call(pk::CallView{});
    pk::PasskeyView passkey;
    passkey.mode = pk::PasskeyMode::Local;
    passkey.title = "Confirm Your Passkey";
    passkey.steps = {{"Tap Confirm Passkey below", ""}, {"Approve", "detail"}, {"Done", "detail"}};
    passkey.confirm_button = true;
    ui.set_sheet(pk::PluginSheet{});
    ui.set_passkey(passkey);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::PasskeyConfirm) != nullptr);
    CHECK(find_target(ui.hit_map(), pk::UiAction::Mute) == nullptr);

    // The debug panel covers everything and only exposes its own controls.
    pk::DebugView debug_view;
    debug_view.open = true;
    debug_view.can_send = true;
    debug_view.rows = {{"Device", "headless kiosk"}, {"Audio Rate", "48000 Hz"}};
    ui.set_debug(debug_view);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugClose) != nullptr);
    CHECK(find_target(ui.hit_map(), pk::UiAction::MicTest) != nullptr);
    CHECK(find_target(ui.hit_map(), pk::UiAction::SendDiagnostics) != nullptr);
    CHECK(count_targets(ui.hit_map(), pk::UiAction::EchoMode) == 3);
    CHECK(find_target(ui.hit_map(), pk::UiAction::Mute) == nullptr);
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugOpen) == nullptr);
}

void test_control_animation() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::Mute) == nullptr);

    pk::CallView active;
    active.phase = pk::CallPhase::Active;
    ui.set_call(active);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    const pk::UiTarget* end0 = find_target(ui.hit_map(), pk::UiAction::End);
    CHECK(end0 != nullptr);
    const float end_start_y = end0->rect.center_y();

    ui.draw(canvas, pk::Size{1280, 720}, 0.2f);
    const pk::UiTarget* end2 = find_target(ui.hit_map(), pk::UiAction::End);
    const pk::UiTarget* mute2 = find_target(ui.hit_map(), pk::UiAction::Mute);
    CHECK(end2 != nullptr && mute2 != nullptr);
    CHECK(end2->rect.center_y() < end_start_y);
    CHECK(mute2->rect.center_y() > end_start_y);

    ui.draw(canvas, pk::Size{1280, 720}, 0.4f);
    const pk::UiTarget* end4 = find_target(ui.hit_map(), pk::UiAction::End);
    const pk::UiTarget* mute4 = find_target(ui.hit_map(), pk::UiAction::Mute);
    CHECK(end4 != nullptr && mute4 != nullptr);
    CHECK(std::fabs(end4->rect.center_y() - 264.0f) < 1.0f);
    CHECK(std::fabs(mute4->rect.center_y() - 488.0f) < 1.0f);
    CHECK(std::fabs(end4->rect.center_x() - end2->rect.center_x()) < 0.5f);
}

void test_media_buttons() {
    FakeCanvas canvas;
    pk::MediaOverlay media(nullptr, "https://ampcode.com", canvas);
    nlohmann::json presentation = {
        {"id", "demo"},
        {"title", "Pictures"},
        {"items",
         nlohmann::json::array({{{"id", "one"}, {"url", "/missing-one.png"}},
                                {{"id", "two"}, {"url", "/missing-two.png"}}})},
    };
    std::string error;
    CHECK(media.set_presentation(presentation, true, &error));
    const pk::MediaButtons buttons = media.buttons(canvas, pk::Rect{0, 0, 1280, 518});
    CHECK(buttons.close.w > 0.0f);
    CHECK(buttons.navigation);
    CHECK(!buttons.can_previous);
    CHECK(buttons.can_next);
    CHECK(buttons.previous.w > 0.0f);
    CHECK(buttons.next.w > 0.0f);
    CHECK(buttons.previous.center_x() < buttons.next.center_x());

    media.next();
    const pk::MediaButtons after = media.buttons(canvas, pk::Rect{0, 0, 1280, 518});
    CHECK(after.can_previous);
    CHECK(!after.can_next);
    media.close();
}

void test_focus_navigation() {
    pk::UiHitMap hits;
    hits.add(pk::Rect{0, 0, 1280, 720}, pk::UiAction::None);
    hits.add(pk::Rect{100, 100, 80, 60}, pk::UiAction::Mute);
    hits.add(pk::Rect{300, 100, 80, 60}, pk::UiAction::End);
    hits.add(pk::Rect{100, 300, 80, 60}, pk::UiAction::Call);
    hits.add(pk::Rect{300, 300, 80, 60}, pk::UiAction::SheetOption, 0);
    hits.add(pk::Rect{500, 100, 80, 60}, pk::UiAction::MediaClose);

    pk::UiFocus focus;
    focus.set_hit_map(&hits);
    focus.sync();
    CHECK(!focus.focused());

    auto press = [&](pk::InputEvent::GamepadButton button, float t) {
        return focus.handle(gamepad_button(button, true), t);
    };

    pk::UiFocusResult result = press(pk::InputEvent::GamepadButton::X, 0.0f);
    CHECK(result.consumed);
    CHECK(result.action == pk::UiAction::None);
    CHECK(focus.focused_action() == pk::UiAction::Mute);  // blocker skipped
    CHECK(focus.focused_index() == -1);

    press(pk::InputEvent::GamepadButton::DPadRight, 0.1f);
    CHECK(focus.focused_action() == pk::UiAction::End);
    press(pk::InputEvent::GamepadButton::DPadDown, 0.2f);
    CHECK(focus.focused_action() == pk::UiAction::SheetOption);
    CHECK(focus.focused_index() == 0);
    press(pk::InputEvent::GamepadButton::DPadRight, 0.3f);
    CHECK(focus.focused_action() == pk::UiAction::MediaClose);
    press(pk::InputEvent::GamepadButton::DPadRight, 0.4f);
    CHECK(focus.focused_action() == pk::UiAction::Mute);  // cycle wraps forward
    press(pk::InputEvent::GamepadButton::DPadUp, 0.5f);
    CHECK(focus.focused_action() == pk::UiAction::MediaClose);  // cycle wraps back
    press(pk::InputEvent::GamepadButton::DPadLeft, 0.6f);
    CHECK(focus.focused_action() == pk::UiAction::End);  // nearest left

    focus.handle(gamepad_axis(pk::InputEvent::GamepadAxis::LeftY, 0.8f), 0.7f);
    CHECK(focus.focused_action() == pk::UiAction::SheetOption);
    focus.handle(gamepad_axis(pk::InputEvent::GamepadAxis::LeftY, 0.9f), 0.8f);
    CHECK(focus.focused_action() == pk::UiAction::SheetOption);  // no repeat while held
    focus.handle(gamepad_axis(pk::InputEvent::GamepadAxis::LeftY, 0.0f), 0.9f);
    focus.handle(gamepad_axis(pk::InputEvent::GamepadAxis::LeftY, -0.8f), 1.0f);
    CHECK(focus.focused_action() == pk::UiAction::End);
    focus.handle(gamepad_axis(pk::InputEvent::GamepadAxis::LeftX, 0.0f), 1.1f);
    focus.handle(gamepad_axis(pk::InputEvent::GamepadAxis::LeftX, 0.8f), 1.2f);
    CHECK(focus.focused_action() == pk::UiAction::MediaClose);

    const pk::UiFocusView view = focus.view(1.2f, true);
    CHECK(view.focused && view.visible);
    CHECK(view.action == pk::UiAction::MediaClose);
    CHECK(focus.focused_rect() != nullptr);

    focus.reset();
    CHECK(!focus.focused());
    CHECK(!focus.hold_active());
}

void test_focus_actions() {
    pk::UiHitMap hits;
    hits.add(pk::Rect{100, 100, 80, 60}, pk::UiAction::Mute);
    hits.add(pk::Rect{300, 100, 80, 60}, pk::UiAction::End);

    pk::UiFocus focus;
    focus.set_hit_map(&hits);
    focus.sync();

    // The first gamepad event focuses the first target; A activates it.
    pk::UiFocusResult result =
        focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 0.0f);
    CHECK(result.consumed);
    CHECK(result.action == pk::UiAction::Mute);
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, false), 0.0f);

    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadRight, true), 0.1f);
    CHECK(focus.focused_action() == pk::UiAction::End);
    result = focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 1.0f);
    CHECK(result.consumed);
    CHECK(result.action == pk::UiAction::None);
    CHECK(focus.hold_active());
    CHECK(std::fabs(focus.hold_progress(1.45f) - 0.5f) < 0.01f);
    result = focus.update(1.45f);
    CHECK(result.action == pk::UiAction::None);
    CHECK(focus.hold_active());
    result = focus.update(1.91f);
    CHECK(result.action == pk::UiAction::End);
    CHECK(!focus.hold_active());
    CHECK(focus.update(2.0f).action == pk::UiAction::None);

    // Releasing A before the hold completes cancels it.
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 3.0f);
    CHECK(focus.hold_active());
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, false), 3.2f);
    CHECK(!focus.hold_active());
    CHECK(focus.update(4.0f).action == pk::UiAction::None);

    // Navigation also cancels a hold.
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 5.0f);
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadLeft, true), 5.1f);
    CHECK(!focus.hold_active());
    CHECK(focus.focused_action() == pk::UiAction::Mute);

    // B dismisses the topmost overlay; Start alone does nothing (Debug needs
    // the Back+Start hold, so a stray Start press cannot open it).
    result = focus.handle(gamepad_button(pk::InputEvent::GamepadButton::B, true), 6.0f);
    CHECK(result.consumed && result.dismiss);
    result = focus.handle(gamepad_button(pk::InputEvent::GamepadButton::Back, true), 6.05f);
    CHECK(result.consumed && result.dismiss);
    result =
        focus.handle(gamepad_button(pk::InputEvent::GamepadButton::Start, true), 6.1f);
    CHECK(!result.toggle_debug);
    CHECK(focus.update(6.5f).toggle_debug == false);
    // Back+Start held together for 1.5s reveals Debug.
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::Back, false), 6.2f);
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::Start, false), 6.2f);
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::Back, true), 7.0f);
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::Start, true), 7.1f);
    CHECK(focus.update(8.0f).toggle_debug == false);
    CHECK(focus.update(8.7f).toggle_debug == true);

    // Pointer press on End starts the same hold; release cancels.
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadRight, true), 7.0f);
    CHECK(focus.focused_action() == pk::UiAction::End);
    result = focus.handle(pointer(pk::InputEvent::Type::PointerDown, 340, 130), 7.1f);
    CHECK(result.consumed && focus.hold_active());
    result = focus.handle(pointer(pk::InputEvent::Type::PointerUp, 340, 130), 7.2f);
    CHECK(result.consumed && !focus.hold_active());

    // A pointer press on a non-End target is left to the pointer router.
    result = focus.handle(pointer(pk::InputEvent::Type::PointerDown, 140, 130), 7.3f);
    CHECK(!result.consumed && !focus.hold_active());

    // Holding a pointer press completes without waiting for release.
    focus.handle(pointer(pk::InputEvent::Type::PointerDown, 340, 130), 8.0f);
    CHECK(focus.hold_active());
    result = focus.update(8.95f);
    CHECK(result.action == pk::UiAction::End);
    CHECK(!focus.hold_active());
    result = focus.handle(pointer(pk::InputEvent::Type::PointerUp, 340, 130), 9.0f);
    CHECK(!result.consumed);

    // A disappearing End target cancels the hold instead of firing.
    CHECK(focus.focused_action() == pk::UiAction::End);
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 10.0f);
    CHECK(focus.hold_active());
    hits.clear();
    hits.add(pk::Rect{100, 100, 80, 60}, pk::UiAction::Mute);
    focus.sync();
    CHECK(focus.focused_action() == pk::UiAction::Mute);
    result = focus.update(10.5f);
    CHECK(result.action == pk::UiAction::None);
    CHECK(!focus.hold_active());
}

void test_focus_occlusion() {
    pk::UiHitMap hits;
    hits.add(pk::Rect{100, 100, 80, 60}, pk::UiAction::MediaClose);
    hits.add(pk::Rect{0, 0, 200, 200}, pk::UiAction::None);  // covers MediaClose
    hits.add(pk::Rect{300, 100, 80, 60}, pk::UiAction::Mute);

    pk::UiFocus focus;
    focus.set_hit_map(&hits);
    focus.sync();
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::X, true), 0.0f);
    CHECK(focus.focused_action() == pk::UiAction::Mute);
}

void test_hint_visibility() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    CHECK(!ui.hints_visible());
    ui.notify_input(gamepad_button(pk::InputEvent::GamepadButton::A, true));
    CHECK(ui.hints_visible());
    ui.notify_input(pointer(pk::InputEvent::Type::PointerMove, 1, 1));
    CHECK(!ui.hints_visible());
    ui.notify_input(gamepad_button(pk::InputEvent::GamepadButton::DPadDown, true));
    CHECK(ui.hints_visible());
    ui.notify_input(pointer(pk::InputEvent::Type::PointerDown, 1, 1));
    CHECK(!ui.hints_visible());
}

void test_client_caption() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    pk::CallView view;
    view.phase = pk::CallPhase::Active;
    view.caption = "On a call with Puck";
    ui.set_call(view);

    // The caption is opt-in; headless and fullscreen keep the old hit map.
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::WindowClose) == nullptr);
    CHECK(ui.hit_map().hit(640.0f, -18.0f) == nullptr);

    ui.set_client_caption(true);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    const pk::UiTarget* close = find_target(ui.hit_map(), pk::UiAction::WindowClose);
    const pk::UiTarget* maximize = find_target(ui.hit_map(), pk::UiAction::WindowMaximize);
    const pk::UiTarget* minimize = find_target(ui.hit_map(), pk::UiAction::WindowMinimize);
    CHECK(close != nullptr);
    CHECK(maximize != nullptr);
    CHECK(minimize != nullptr);
    if (!close || !maximize || !minimize) {
        return;
    }
    // Buttons are 40x28 and right-aligned, in content space above y=0.
    CHECK(std::fabs(close->rect.w - pk::CarUi::kCaptionButtonWidth) < 0.01f);
    CHECK(std::fabs(close->rect.h - pk::CarUi::kCaptionButtonHeight) < 0.01f);
    CHECK(std::fabs(close->rect.y + pk::CarUi::kCaptionHeight -
                    (pk::CarUi::kCaptionHeight - pk::CarUi::kCaptionButtonHeight) * 0.5f) <
          0.01f);
    CHECK(close->rect.x > maximize->rect.x);
    CHECK(maximize->rect.x > minimize->rect.x);
    CHECK(std::fabs(close->rect.x + close->rect.w -
                    (1280.0f - pk::CarUi::kCaptionRightMargin)) < 0.01f);
    CHECK(close->rect.x + close->rect.w <= 1280.0f);

    // The empty strip is a blocker; each button center is its own action.
    const pk::UiTarget* blocker = ui.hit_map().hit(640.0f, -18.0f);
    CHECK(blocker != nullptr);
    CHECK(blocker && blocker->action == pk::UiAction::None);
    CHECK(ui.hit_map().hit(close->rect.center_x(), -18.0f)->action ==
          pk::UiAction::WindowClose);
    CHECK(ui.hit_map().hit(maximize->rect.center_x(), -18.0f)->action ==
          pk::UiAction::WindowMaximize);
    CHECK(ui.hit_map().hit(minimize->rect.center_x(), -18.0f)->action ==
          pk::UiAction::WindowMinimize);

    // A click on the strip never reaches the Puck; a button click emits one
    // window action through the normal router.
    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    router.handle(pointer(pk::InputEvent::Type::PointerDown, 640.0f, -18.0f));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, 640.0f, -18.0f)) ==
          pk::UiAction::None);
    router.handle(pointer(pk::InputEvent::Type::PointerDown, close->rect.center_x(), -18.0f));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, close->rect.center_x(),
                                -18.0f)) == pk::UiAction::WindowClose);

    // Screen targets stay below the strip, so a Puck grab at y=-18 cannot
    // reach them.
    const pk::UiTarget* mute = find_target(ui.hit_map(), pk::UiAction::Mute);
    CHECK(mute != nullptr);
    CHECK(mute && mute->rect.y >= 0.0f);

    // Gamepad focus skips the pointer-only window controls.
    pk::UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    focus.sync();
    for (int i = 0; i < 12; ++i) {
        focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadRight, true),
                     0.1f * static_cast<float>(i));
        focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadRight, false),
                     0.1f * static_cast<float>(i) + 0.01f);
        const pk::UiAction action = focus.focused_action();
        CHECK(action != pk::UiAction::WindowMinimize);
        CHECK(action != pk::UiAction::WindowMaximize);
        CHECK(action != pk::UiAction::WindowClose);
    }

    // The hidden Debug corner stays at the bottom-left of the content, which is
    // the bottom-left of the window.
    for (int i = 0; i < 5; ++i) {
        pk::InputEvent tap;
        tap.type = pk::InputEvent::Type::PointerDown;
        tap.x = 20.0f;
        tap.y = 720.0f - pk::CarUi::kCaptionHeight - 20.0f;
        ui.notify_input(tap);
        tap.type = pk::InputEvent::Type::PointerUp;
        ui.notify_input(tap);
        ui.draw(canvas, pk::Size{1280, 720},
                0.1f + 0.1f * static_cast<float>(i));
    }
    CHECK(find_target(ui.hit_map(), pk::UiAction::DebugOpen) != nullptr);

    CHECK(std::string(pk::ui_action_name(pk::UiAction::WindowMinimize)) ==
          "window-minimize");
    CHECK(std::string(pk::ui_action_name(pk::UiAction::WindowMaximize)) ==
          "window-maximize");
    CHECK(std::string(pk::ui_action_name(pk::UiAction::WindowClose)) == "window-close");
}

const pk::UiTarget* find_visible_option(const pk::UiHitMap& hits, int index,
                                        pk::Size size) {
    const pk::UiTarget* target = find_indexed_target(hits, pk::UiAction::SheetOption, index);
    if (!target || target->rect.y < 0.0f ||
        target->rect.y + target->rect.h > static_cast<float>(size.height)) {
        return nullptr;
    }
    return target;
}

void test_text_keys_do_not_navigate() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.request_id = "text-keys";
    sheet.title = "Pick a destination";
    sheet.message = "Choose where Puck should take you next";
    for (int i = 0; i < 12; ++i) {
        sheet.options.push_back("Option " + std::to_string(i));
    }
    ui.set_sheet(sheet);
    const pk::Size size{420, 640};
    ui.draw(canvas, size, 0.0f);
    CHECK(ui.scroll_sheet(100));
    ui.draw(canvas, size, 0.1f);
    CHECK(find_visible_option(ui.hit_map(), 11, size) != nullptr);

    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    pk::InputEvent text_end;
    text_end.type = pk::InputEvent::Type::KeyDown;
    text_end.key = 35;  // '#' and VK_END share this numeric value.
    text_end.text = true;
    ui.notify_input(text_end);
    CHECK(router.handle(text_end) == pk::UiAction::None);
    ui.draw(canvas, size, 0.2f);
    CHECK(find_visible_option(ui.hit_map(), 0, size) == nullptr);

    pk::InputEvent text_home;
    text_home.type = pk::InputEvent::Type::KeyDown;
    text_home.key = 102;  // 'f' and evdev KEY_HOME share this numeric value.
    text_home.text = true;
    ui.notify_input(text_home);
    router.set_hit_map(&ui.hit_map());
    CHECK(router.handle(text_home) == pk::UiAction::None);
    ui.draw(canvas, size, 0.3f);
    CHECK(find_visible_option(ui.hit_map(), 11, size) != nullptr);

    // Printable '{' shares 123 with F12. Text must not reveal Debug; the
    // real F12 (text=false) still does.
    FakePuck debug_puck;
    FakeCanvas debug_canvas;
    pk::CarUi debug_ui(debug_puck);
    debug_ui.set_pairing(pk::QrCode{}, "Call Puck", "Waiting for scan");
    debug_ui.draw(debug_canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(find_target(debug_ui.hit_map(), pk::UiAction::DebugOpen) == nullptr);

    pk::InputEvent text_brace;
    text_brace.type = pk::InputEvent::Type::KeyDown;
    text_brace.key = 123;
    text_brace.text = true;
    debug_ui.notify_input(text_brace);
    debug_ui.draw(debug_canvas, pk::Size{1280, 720}, 0.1f);
    CHECK(find_target(debug_ui.hit_map(), pk::UiAction::DebugOpen) == nullptr);

    pk::InputEvent text_x;
    text_x.type = pk::InputEvent::Type::KeyDown;
    text_x.key = 88;
    text_x.text = true;
    debug_ui.notify_input(text_x);
    debug_ui.draw(debug_canvas, pk::Size{1280, 720}, 0.2f);
    CHECK(find_target(debug_ui.hit_map(), pk::UiAction::DebugOpen) == nullptr);

    pk::InputEvent f12;
    f12.type = pk::InputEvent::Type::KeyDown;
    f12.key = 123;
    f12.text = false;
    debug_ui.notify_input(f12);
    debug_ui.draw(debug_canvas, pk::Size{1280, 720}, 0.3f);
    CHECK(find_target(debug_ui.hit_map(), pk::UiAction::DebugOpen) != nullptr);
}

void test_pairing_retry() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_pairing(pk::QrCode{}, "Confirm on your phone", "", "482 913");
    ui.set_pairing_recovery(pk::PairingRecovery::Retry);
    const pk::Size size{420, 640};
    canvas.texts.clear();
    ui.draw(canvas, size, 0.0f);

    const pk::UiTarget* retry = find_target(ui.hit_map(), pk::UiAction::PairingRetry);
    CHECK(retry != nullptr);
    CHECK(ui.pairing_retry_bounds().w > 0.0f);
    CHECK(has_text(canvas, "Try Again"));
    CHECK(!has_text(canvas, "Or Sign In Manually"));
    if (!retry) {
        return;
    }
    CHECK(retry->rect.x >= 0.0f);
    CHECK(retry->rect.y >= 0.0f);
    CHECK(retry->rect.x + retry->rect.w <= static_cast<float>(size.width));
    CHECK(retry->rect.y + retry->rect.h <= static_cast<float>(size.height));
    pk::UiFocus retry_focus;
    retry_focus.set_hit_map(&ui.hit_map());
    retry_focus.sync();
    retry_focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 0.0f);
    CHECK(retry_focus.focused_action() == pk::UiAction::PairingRetry);

    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    router.handle(pointer(pk::InputEvent::Type::PointerDown, retry->rect.center_x(),
                          retry->rect.center_y()));
    int retries = 0;
    pk::UiActionHandlers handlers;
    handlers.pairing_retry = [&]() { ++retries; };
    const pk::UiAction action = router.handle(
        pointer(pk::InputEvent::Type::PointerUp, retry->rect.center_x(),
                retry->rect.center_y()));
    CHECK(action == pk::UiAction::PairingRetry);
    CHECK(pk::dispatch_ui_action(action, -1, handlers));
    CHECK(retries == 1);

    ui.set_pairing_recovery(pk::PairingRecovery::Retrying);
    canvas.texts.clear();
    ui.draw(canvas, size, 0.1f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::PairingRetry) == nullptr);
    CHECK(ui.pairing_retry_bounds().w > 0.0f);
    CHECK(has_text_containing(canvas, "Retrying"));

    // A failed attempt keeps the status line and offers the same button.
    ui.set_pairing(pk::QrCode{}, "Call Puck", "Pairing failed", "");
    ui.set_pairing_recovery(pk::PairingRecovery::Retry);
    ui.draw(canvas, size, 0.2f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::PairingRetry) != nullptr);
    CHECK(ui.status_bounds().w > 0.0f);
    const pk::UiTarget* failed_retry =
        find_target(ui.hit_map(), pk::UiAction::PairingRetry);
    CHECK(failed_retry && !overlaps(failed_retry->rect, ui.status_bounds()));

    // The short landscape pairing screen shrinks the card instead of
    // clipping the title, code, status, or recovery button.
    ui.set_pairing(pk::QrCode{}, "Confirm on your phone", "", "482 913");
    ui.set_pairing_recovery(pk::PairingRecovery::Retry);
    const pk::Size short_size{360, 240};
    ui.draw(canvas, short_size, 0.3f);
    const pk::UiTarget* short_retry =
        find_target(ui.hit_map(), pk::UiAction::PairingRetry);
    CHECK(short_retry != nullptr);
    if (short_retry) {
        CHECK(short_retry->rect.y >= 0.0f);
        CHECK(short_retry->rect.y + short_retry->rect.h <=
              static_cast<float>(short_size.height));
        CHECK(short_retry->rect.x + short_retry->rect.w <=
              static_cast<float>(short_size.width));
    }

    ui.set_pairing_recovery(pk::PairingRecovery::None);
    ui.draw(canvas, size, 0.4f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::PairingRetry) == nullptr);
    CHECK(ui.pairing_retry_bounds().w == 0.0f);
}

void test_call_availability() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    ui.set_call_available(false);
    const pk::Size size{1280, 720};
    ui.draw(canvas, size, 0.0f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::Call) == nullptr);
    CHECK(!ui.call_available());

    pk::UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    focus.sync();
    CHECK(!focus.focused());
    focus.handle(gamepad_button(pk::InputEvent::GamepadButton::A, true), 0.0f);
    CHECK(focus.focused_action() != pk::UiAction::Call);

    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerDown, 640.0f, 360.0f)) ==
          pk::UiAction::None);
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, 640.0f, 360.0f)) ==
          pk::UiAction::None);

    // The same view becomes actionable only when the gate opens.
    ui.set_call_available(true);
    ui.draw(canvas, size, 0.1f);
    const pk::UiTarget* call = find_target(ui.hit_map(), pk::UiAction::Call);
    CHECK(call != nullptr);
    if (!call) {
        return;
    }
    router.set_hit_map(&ui.hit_map());
    router.handle(pointer(pk::InputEvent::Type::PointerDown, call->rect.center_x(),
                          call->rect.center_y()));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, call->rect.center_x(),
                                call->rect.center_y())) == pk::UiAction::Call);
}

void test_mic_meter() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    pk::CallView view;
    view.phase = pk::CallPhase::Active;
    view.input_level = 0.05f;
    ui.set_call(view);

    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(ui.mic_level() > 0.9f);

    // A halt falls slowly instead of blinking with the output...
    view.input_level = 0.0f;
    ui.set_call(view);
    ui.draw(canvas, pk::Size{1280, 720}, 0.05f);
    CHECK(ui.mic_level() > 0.55f);
    ui.draw(canvas, pk::Size{1280, 720}, 1.2f);
    CHECK(ui.mic_level() < 0.08f);
    ui.draw(canvas, pk::Size{1280, 720}, 1.8f);
    CHECK(ui.mic_level() < 0.02f);

    // ...while a quiet voice still leaves a visible fill.
    view.input_level = 0.004f;
    ui.set_call(view);
    ui.draw(canvas, pk::Size{1280, 720}, 2.0f);
    CHECK(ui.mic_level() > 0.2f);

    // Muted, connecting, and ended frames snap to zero.
    view.muted = true;
    view.phase = pk::CallPhase::Muted;
    ui.set_call(view);
    ui.draw(canvas, pk::Size{1280, 720}, 3.0f);
    CHECK(ui.mic_level() == 0.0f);

    view.muted = false;
    view.phase = pk::CallPhase::Connecting;
    ui.set_call(view);
    ui.draw(canvas, pk::Size{1280, 720}, 4.0f);
    CHECK(ui.mic_level() == 0.0f);

    view.phase = pk::CallPhase::Ended;
    view.input_level = 0.05f;
    ui.set_call(view);
    ui.draw(canvas, pk::Size{1280, 720}, 5.0f);
    CHECK(ui.mic_level() == 0.0f);
}

void test_call_state_text() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);

    pk::CallView active;
    active.phase = pk::CallPhase::Active;
    active.caption = "On a call with Puck";
    active.detail = "Puck is listening";
    ui.set_call(active);
    canvas.texts.clear();
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(!has_text(canvas, "On a call with Puck"));
    CHECK(!has_text(canvas, "Puck is listening"));

    pk::CallView speaking = active;
    speaking.phase = pk::CallPhase::Speaking;
    speaking.caption = "Puck is speaking";
    speaking.detail = "I can help with that.";
    speaking.speaking = true;
    ui.set_call(speaking);
    canvas.texts.clear();
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(!has_text(canvas, "Puck is speaking"));
    CHECK(!has_text(canvas, "I can help with that."));

    pk::CallView muted = active;
    muted.phase = pk::CallPhase::Muted;
    muted.muted = true;
    muted.caption = "Microphone off";
    ui.set_call(muted);
    canvas.texts.clear();
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(has_text(canvas, "Microphone off"));

    pk::CallView failed = active;
    failed.phase = pk::CallPhase::Failed;
    failed.detail = "WebRTC connect timeout";
    ui.set_call(failed);
    canvas.texts.clear();
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(has_text(canvas, "WebRTC connect timeout"));
}

void test_puck_visual_passthrough() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);

    pk::PuckVisual visual;
    visual.listening = true;
    visual.speaking = false;
    visual.mouth = 0.0f;
    ui.set_visual(visual);

    pk::CallView stale;
    stale.phase = pk::CallPhase::Speaking;
    stale.speaking = true;
    stale.thinking = true;
    stale.mouth = 0.9f;
    ui.set_call(stale);
    ui.draw(canvas, pk::Size{1280, 720}, 0.0f);
    CHECK(!puck.last_visual.speaking);
    CHECK(puck.last_visual.listening);
    CHECK(!puck.last_visual.thinking);
    CHECK(puck.last_visual.mouth == 0.0f);

    visual.speaking = true;
    visual.listening = false;
    visual.mouth = 0.6f;
    ui.set_visual(visual);
    pk::CallView active;
    active.phase = pk::CallPhase::Active;
    ui.set_call(active);
    ui.draw(canvas, pk::Size{1280, 720}, 0.1f);
    CHECK(puck.last_visual.speaking);
    CHECK(!puck.last_visual.listening);
    CHECK(puck.last_visual.mouth == 0.6f);
}

void test_long_sheet_scroll() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    pk::CallView view;
    view.phase = pk::CallPhase::Active;
    ui.set_call(view);

    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.request_id = "long-list";
    sheet.title = "Pick a destination";
    sheet.message = "Choose where Puck should take you next";
    for (int i = 0; i < 12; ++i) {
        sheet.options.push_back("Option " + std::to_string(i));
    }
    sheet.options.back() = "Last option: the quiet overlook";
    ui.set_sheet(sheet);
    const pk::Size size{420, 640};
    ui.draw(canvas, size, 0.0f);

    const pk::Rect up = ui.sheet_scroll_up();
    const pk::Rect down = ui.sheet_scroll_down();
    CHECK(up.w > 0.0f && down.w > 0.0f);
    const pk::UiTarget* dismiss = find_target(ui.hit_map(), pk::UiAction::SheetDismiss);
    CHECK(dismiss != nullptr);
    if (!dismiss) {
        return;
    }

    for (const pk::UiTarget& target : ui.hit_map().targets()) {
        if (target.action != pk::UiAction::SheetOption || target.rect.y < 0.0f ||
            target.rect.y + target.rect.h > static_cast<float>(size.height)) {
            continue;
        }
        CHECK(target.rect.y + target.rect.h <= dismiss->rect.y);
        CHECK(!overlaps(target.rect, dismiss->rect));
    }

    // The rail owns its taps; the router must not fire an option behind it.
    CHECK(ui.hit_map().hit(down.center_x(), down.center_y()) != nullptr);
    CHECK(ui.hit_map().hit(down.center_x(), down.center_y())->action ==
          pk::UiAction::SheetScrollDown);
    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    router.handle(pointer(pk::InputEvent::Type::PointerDown, down.center_x(),
                          down.center_y()));
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, down.center_x(),
                                down.center_y())) == pk::UiAction::SheetScrollDown);
    const pk::UiTarget* first_before = find_visible_option(ui.hit_map(), 0, size);
    CHECK(first_before != nullptr);
    const float first_y = first_before ? first_before->rect.y : 0.0f;

    pk::InputEvent tap;
    tap.type = pk::InputEvent::Type::PointerDown;
    tap.x = down.center_x();
    tap.y = down.center_y();
    ui.notify_input(tap);
    ui.draw(canvas, size, 0.1f);
    CHECK(find_visible_option(ui.hit_map(), 0, size) == nullptr);
    const pk::UiTarget* second = find_visible_option(ui.hit_map(), 1, size);
    CHECK(second != nullptr);
    CHECK(second && std::fabs(second->rect.y - first_y) < 0.5f);
    if (second) {
        router.set_hit_map(&ui.hit_map());
        router.handle(pointer(pk::InputEvent::Type::PointerDown, second->rect.center_x(),
                              second->rect.center_y()));
        int index = -1;
        CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp,
                                    second->rect.center_x(), second->rect.center_y()),
                            &index) == pk::UiAction::SheetOption);
        CHECK(index == 1);
    }

    // Scrolling to the end keeps the final row and Dismiss reachable.
    CHECK(ui.scroll_sheet(100));
    ui.draw(canvas, size, 0.2f);
    const pk::UiTarget* last = find_visible_option(ui.hit_map(), 11, size);
    CHECK(last != nullptr);
    dismiss = find_target(ui.hit_map(), pk::UiAction::SheetDismiss);
    CHECK(dismiss != nullptr);
    if (last && dismiss) {
        CHECK(last->rect.y + last->rect.h <= dismiss->rect.y);
        CHECK(!overlaps(last->rect, dismiss->rect));
    }
    CHECK(ui.sheet_scroll_down().w > 0.0f);  // the rail stays, its down arrow is disabled
}

void test_long_labels_keep_tail() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.request_id = "same-prefix";
    sheet.title = "Pick a destination";
    sheet.options = {
        "The riverside park with a long and nearly identical label, EAST",
        "The riverside park with a long and nearly identical label, WEST",
    };
    ui.set_sheet(sheet);
    ui.draw(canvas, pk::Size{420, 640}, 0.0f);
    CHECK(has_text_containing(canvas, "EAST"));
    CHECK(has_text_containing(canvas, "WEST"));
}

void test_small_window_sheet_separation() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    pk::CallView view;
    view.phase = pk::CallPhase::Active;
    ui.set_call(view);
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.request_id = "small-list";
    sheet.title = "Pick a destination";
    for (int i = 0; i < 12; ++i) {
        sheet.options.push_back("Option " + std::to_string(i));
    }
    ui.set_sheet(sheet);
    const pk::Size size{360, 240};
    ui.draw(canvas, size, 0.0f);

    const pk::UiTarget* option = find_visible_option(ui.hit_map(), 0, size);
    const pk::UiTarget* dismiss = find_target(ui.hit_map(), pk::UiAction::SheetDismiss);
    const pk::UiTarget* end = find_target(ui.hit_map(), pk::UiAction::End);
    const pk::UiTarget* mute = find_target(ui.hit_map(), pk::UiAction::Mute);
    const pk::Rect up = ui.sheet_scroll_up();
    const pk::Rect down = ui.sheet_scroll_down();
    CHECK(option != nullptr);
    CHECK(dismiss != nullptr);
    CHECK(end != nullptr);
    CHECK(mute != nullptr);
    CHECK(up.w > 0.0f && up.h > 0.0f);
    CHECK(down.w > 0.0f && down.h > 0.0f);
    if (!option || !dismiss || !end || !mute) {
        return;
    }
    CHECK(!overlaps(option->rect, up));
    CHECK(!overlaps(option->rect, down));
    CHECK(!overlaps(up, down));
    CHECK(!overlaps(dismiss->rect, up));
    CHECK(!overlaps(dismiss->rect, down));
    CHECK(!overlaps(option->rect, dismiss->rect));
    CHECK(!overlaps(end->rect, option->rect));
    CHECK(!overlaps(end->rect, up));
    CHECK(!overlaps(end->rect, down));
    CHECK(!overlaps(end->rect, dismiss->rect));
    CHECK(!overlaps(mute->rect, option->rect));
    CHECK(!overlaps(mute->rect, dismiss->rect));

    CHECK(ui.hit_map().hit(option->rect.center_x(), option->rect.center_y())->action ==
          pk::UiAction::SheetOption);
    CHECK(ui.hit_map().hit(up.center_x(), up.center_y())->action ==
          pk::UiAction::SheetScrollUp);
    CHECK(ui.hit_map().hit(down.center_x(), down.center_y())->action ==
          pk::UiAction::SheetScrollDown);
    CHECK(ui.hit_map().hit(dismiss->rect.center_x(), dismiss->rect.center_y())->action ==
          pk::UiAction::SheetDismiss);
    CHECK(ui.hit_map().hit(end->rect.center_x(), end->rect.center_y())->action ==
          pk::UiAction::End);
}

void test_sheet_scroll_action_refreshes_route() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.request_id = "end-key-list";
    sheet.title = "Pick a destination";
    sheet.message = "Choose where Puck should take you next";
    for (int i = 0; i < 12; ++i) {
        sheet.options.push_back("Option " + std::to_string(i));
    }
    ui.set_sheet(sheet);
    const pk::Size size{420, 640};
    ui.draw(canvas, size, 0.0f);

    pk::UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    pk::InputEvent end_key;
    end_key.type = pk::InputEvent::Type::KeyDown;
    end_key.key = 35;
    ui.notify_input(end_key);
    CHECK(router.handle(end_key) == pk::UiAction::SheetScrollDown);
    CHECK(find_visible_option(ui.hit_map(), 11, size) == nullptr);
    pk::UiActionHandlers handlers;
    CHECK(pk::dispatch_ui_action(pk::UiAction::SheetScrollDown, -1, handlers));

    // The integration's minimal refresh: the next event sees the new map.
    ui.draw(canvas, size, 0.1f);
    const pk::UiTarget* last = find_visible_option(ui.hit_map(), 11, size);
    CHECK(last != nullptr);
    if (!last) {
        return;
    }
    CHECK(std::fabs(last->rect.center_x() - 186.0f) < 1.0f);
    CHECK(std::fabs(last->rect.center_y() - 332.0f) < 1.0f);
    router.set_hit_map(&ui.hit_map());
    router.handle(pointer(pk::InputEvent::Type::PointerDown, 186.0f, 332.0f));
    int index = -1;
    CHECK(router.handle(pointer(pk::InputEvent::Type::PointerUp, 186.0f, 332.0f),
                        &index) == pk::UiAction::SheetOption);
    CHECK(index == 11);
}

void test_long_sheet_focus_reaches_last() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.request_id = "focus-list";
    sheet.title = "Pick a destination";
    for (int i = 0; i < 12; ++i) {
        sheet.options.push_back("Option " + std::to_string(i));
    }
    ui.set_sheet(sheet);
    const pk::Size size{420, 640};
    ui.draw(canvas, size, 0.0f);

    pk::UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    focus.sync();
    float now = 0.1f;

    // The bumpers page the list. After two pages the last four options are
    // visible, so DPad focus can reach the final one.
    for (int page = 0; page < 2; ++page) {
        ui.notify_input(gamepad_button(pk::InputEvent::GamepadButton::RightBumper, true));
        ui.notify_input(gamepad_button(pk::InputEvent::GamepadButton::RightBumper, false));
        ui.draw(canvas, size, now);
        focus.sync();
        now += 0.05f;
    }
    CHECK(find_visible_option(ui.hit_map(), 11, size) != nullptr);

    for (int step = 0; step < 10 && focus.focused_index() != 11; ++step) {
        focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadDown, true), now);
        focus.handle(gamepad_button(pk::InputEvent::GamepadButton::DPadDown, false),
                     now + 0.01f);
        ui.set_focus(focus.view(now + 0.02f, true));
        ui.draw(canvas, size, now + 0.03f);
        focus.sync();
        now += 0.05f;
    }
    CHECK(focus.focused_action() == pk::UiAction::SheetOption);
    CHECK(focus.focused_index() == 11);
    CHECK(find_visible_option(ui.hit_map(), 11, size) != nullptr);
}

void test_confirm_and_phone_sheets() {
    FakePuck puck;
    FakeCanvas canvas;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    const pk::Size size{420, 640};

    pk::PluginSheet confirm;
    confirm.kind = pk::PluginSheetKind::Confirm;
    confirm.request_id = "confirm";
    confirm.title = "Delete the recording?";
    confirm.confirm_label = "Delete";
    ui.set_sheet(confirm);
    ui.draw(canvas, size, 0.0f);
    CHECK(count_targets(ui.hit_map(), pk::UiAction::SheetOption) == 2);
    const pk::UiTarget* dismiss = find_target(ui.hit_map(), pk::UiAction::SheetDismiss);
    CHECK(dismiss != nullptr);
    for (const pk::UiTarget& target : ui.hit_map().targets()) {
        if (target.action == pk::UiAction::SheetOption) {
            CHECK(target.rect.y >= 0.0f);
            CHECK(target.rect.y + target.rect.h <= static_cast<float>(size.height));
            CHECK(dismiss && !overlaps(target.rect, dismiss->rect));
        }
    }
    CHECK(ui.sheet_scroll_up().w == 0.0f);

    pk::PluginSheet phone;
    phone.kind = pk::PluginSheetKind::Phone;
    phone.request_id = "phone";
    phone.title = "Approve on your phone";
    ui.set_sheet(phone);
    ui.draw(canvas, size, 0.1f);
    CHECK(find_target(ui.hit_map(), pk::UiAction::SheetOk) != nullptr);
    CHECK(count_targets(ui.hit_map(), pk::UiAction::SheetOption) == 0);
    CHECK(find_target(ui.hit_map(), pk::UiAction::SheetDismiss) == nullptr);
}

void test_gamepad_script() {
    const std::string text = R"([
        {"t": 0.3, "type": "gamepad", "axis": "left_x", "value": 0.75},
        {"t": 0.1, "type": "gamepad", "button": "a", "state": "down"},
        {"t": 0.2, "type": "gamepad", "button": "dpad_up", "state": "up"},
        {"t": 0.4, "type": "wait"}
    ])";
    std::vector<pk::ScriptedInput> events;
    std::string error;
    CHECK_MSG(pk::parse_input_script(text, &events, &error), error);
    CHECK(events.size() == 4);
    CHECK(events[0].event.type == pk::InputEvent::Type::GamepadButtonDown);
    CHECK(events[0].event.button == pk::InputEvent::GamepadButton::A);
    CHECK(events[1].event.type == pk::InputEvent::Type::GamepadButtonUp);
    CHECK(events[1].event.button == pk::InputEvent::GamepadButton::DPadUp);
    CHECK(events[2].event.type == pk::InputEvent::Type::GamepadAxis);
    CHECK(events[2].event.axis == pk::InputEvent::GamepadAxis::LeftX);
    CHECK(std::fabs(events[2].event.value - 0.75f) < 0.001f);
    CHECK(events[3].event.type == pk::InputEvent::Type::None);
    CHECK(events[3].t == 0.4);

    std::vector<pk::ScriptedInput> bad;
    CHECK(!pk::parse_input_script(
        "[{\"type\":\"gamepad\",\"button\":\"nope\",\"state\":\"down\"}]", &bad, &error));
    CHECK(!pk::parse_input_script(
        "[{\"type\":\"gamepad\",\"button\":\"a\",\"state\":\"sideways\"}]", &bad, &error));
    CHECK(!pk::parse_input_script(
        "[{\"type\":\"gamepad\",\"axis\":\"left_z\",\"value\":0.5}]", &bad, &error));
    CHECK(!pk::parse_input_script(
        "[{\"type\":\"gamepad\",\"axis\":\"left_x\",\"value\":2}]", &bad, &error));
    CHECK(!pk::parse_input_script("[{\"type\":\"gamepad\"}]", &bad, &error));
    CHECK(!pk::parse_input_script("[{\"type\":\"wait\"}]", &bad, &error));
}

}  // namespace

int main() {
    test_hit_map();
    test_router();
    test_script();
    test_gamepad_script();
    test_dispatch();
    test_plugin_ui();
    test_passkey();
    test_diagnostics();
    test_car_ui_targets();
    test_control_animation();
    test_media_buttons();
    test_focus_navigation();
    test_focus_actions();
    test_focus_occlusion();
    test_hint_visibility();
    test_client_caption();
    test_text_keys_do_not_navigate();
    test_pairing_retry();
    test_call_availability();
    test_mic_meter();
    test_call_state_text();
    test_puck_visual_passthrough();
    test_long_sheet_scroll();
    test_long_labels_keep_tail();
    test_small_window_sheet_separation();
    test_sheet_scroll_action_refreshes_route();
    test_long_sheet_focus_reaches_last();
    test_confirm_and_phone_sheets();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
