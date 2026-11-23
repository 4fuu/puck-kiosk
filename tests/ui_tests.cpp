#include "app/car_ui.h"
#include "app/diagnostics.h"
#include "app/media_overlay.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
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
    void text(float, float, float, pk::Color, pk::TextAlign, const std::string&) override {}
    float measure_text(float size, const std::string& value) override {
        return static_cast<float>(value.size()) * size * 0.5f;
    }
};

class FakePuck final : public pk::PuckView {
public:
    const char* name() const override { return "fake"; }
    bool is_3d() const override { return false; }
    bool initialize(std::string*) override { return true; }
    void draw(pk::Canvas&, pk::Rect, const pk::PuckVisual&, float) override {}
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
    pk::UiActionHandlers handlers;
    handlers.call = [&]() { ++call; };
    handlers.mute = [&]() { ++mute; };
    handlers.end = [&]() { ++end; };
    handlers.media_close = [&]() { ++media_close; };
    handlers.sheet_option = [&](int index) { option = index; };
    handlers.echo_mode = [&](int index) { echo = index; };
    handlers.passkey_confirm = [&]() { ++passkey; };

    CHECK(pk::dispatch_ui_action(pk::UiAction::Call, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::Mute, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::End, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::MediaClose, -1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::SheetOption, 2, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::EchoMode, 1, handlers));
    CHECK(pk::dispatch_ui_action(pk::UiAction::PasskeyConfirm, -1, handlers));
    CHECK(call == 1 && mute == 1 && end == 1 && media_close == 1);
    CHECK(option == 2 && echo == 1 && passkey == 1);
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

    // A later snapshot must not resurrect a locally hidden request.
    plugins.on_actor_message(plugin_event("plugin.ui.snapshot", {{"requests", requests}}).dump());
    CHECK(plugins.sheet().kind == pk::PluginSheetKind::Select);  // r1 is pending again

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

    const pk::UiTarget* mute = find_target(ui.hit_map(), pk::UiAction::Mute);
    const pk::UiTarget* end = find_target(ui.hit_map(), pk::UiAction::End);
    const pk::UiTarget* debug = find_target(ui.hit_map(), pk::UiAction::DebugOpen);
    CHECK(mute != nullptr);
    CHECK(end != nullptr);
    CHECK(debug != nullptr);
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

}  // namespace

int main() {
    test_hit_map();
    test_router();
    test_script();
    test_dispatch();
    test_plugin_ui();
    test_passkey();
    test_diagnostics();
    test_car_ui_targets();
    test_media_buttons();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
