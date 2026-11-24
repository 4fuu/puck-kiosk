#include "app/app.h"

#include "app/call_flow.h"
#include "app/car_ui.h"
#include "app/diagnostics.h"
#include "app/media_overlay.h"
#include "app/pairing.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
#include "app/ui_focus.h"
#include "app/ui_input.h"
#include "core/log.h"
#include "platform/audio.h"
#include "platform/http.h"
#include "puck/puck_interaction.h"
#include "puck/puck_view.h"
#include "render/renderer.h"
#include "voice/aec.h"
#include "voice/engine.h"
#include "voice/loopback.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pk {
namespace {

using Clock = std::chrono::steady_clock;

// Pairing renews the car session on the UI thread while the media worker
// fetches pictures. The HttpClient seam is documented as sequential, so this
// wrapper serializes both callers instead of changing a platform backend.
class SerializedHttpClient final : public HttpClient {
public:
    explicit SerializedHttpClient(std::unique_ptr<HttpClient> inner) : inner_(std::move(inner)) {}

    HttpResponse request(const HttpRequest& request, std::string* error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->request(request, error);
    }

    std::string export_cookies() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->export_cookies();
    }

    void import_cookies(const std::string& cookies) override {
        std::lock_guard<std::mutex> lock(mutex_);
        inner_->import_cookies(cookies);
    }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<HttpClient> inner_;
};

void log_actor_message(const std::string& text) {
    if (text.size() > 64 * 1024) {
        const std::regex method_re(R"re("method"\s*:\s*"([^"]+)")re");
        std::smatch match;
        const std::string prefix = text.substr(0, 512);
        if (std::regex_search(prefix, match, method_re)) {
            log::info("actor <- %s (payload %zu bytes)", match[1].str().c_str(), text.size());
        } else {
            log::info("actor <- (payload %zu bytes)", text.size());
        }
        return;
    }
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        log::info("actor <- %s", redact_text(text, 300).c_str());
        return;
    }
    if (parsed.is_array()) {
        std::string types;
        for (const auto& entry : parsed) {
            if (entry.contains("type") && entry["type"].is_string()) {
                if (!types.empty()) {
                    types += ",";
                }
                types += entry["type"].get<std::string>();
            }
        }
        log::info("actor <- batch of %zu [%s]", parsed.size(), types.c_str());
        return;
    }
    if (parsed.contains("method") && parsed["method"].is_string()) {
        const std::string method = parsed["method"].get<std::string>();
        const std::string params =
            parsed.contains("params") ? redact_json(parsed["params"].dump(), 300) : "{}";
        log::info("actor <- %s %s", method.c_str(), params.c_str());
        return;
    }
    if (parsed.contains("id") && (parsed.contains("result") || parsed.contains("error"))) {
        log::info("actor <- response id=%s %s", parsed["id"].dump().c_str(),
                  redact_json(text, 300).c_str());
        return;
    }
    log::info("actor <- %s", redact_json(text, 300).c_str());
}

// The actor socket's poll() waits up to a second for the next frame. Reading it
// on the UI thread would hold touch input behind the next actor message, so a
// reader thread owns the socket and the UI drains a queue instead.
class ActorReader {
public:
    ~ActorReader() { stop(); }

    void start(WebSocket* socket) {
        stop();
        socket_ = socket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = false;
            closed_ = false;
            error_.clear();
            messages_.clear();
        }
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!thread_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        if (socket_) {
            socket_->close();
        }
        thread_.join();
        socket_ = nullptr;
    }

    bool take(std::string* message, std::string* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!messages_.empty()) {
            *message = std::move(messages_.front());
            messages_.pop_front();
            return true;
        }
        if (!error_.empty() && error) {
            *error = error_;
            error_.clear();
        }
        return false;
    }

    bool drained_and_closed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && messages_.empty();
    }

private:
    void run() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            std::string message;
            std::string error;
            if (socket_->poll(message, &error)) {
                std::lock_guard<std::mutex> lock(mutex_);
                messages_.push_back(std::move(message));
                continue;
            }
            if (!error.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = error;
            }
            if (!socket_->is_open()) {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
                return;
            }
        }
    }

    WebSocket* socket_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::deque<std::string> messages_;
    std::string error_;
    bool stopping_ = false;
    bool closed_ = false;
};

bool load_text_spec(const std::string& spec, std::string* text, std::string* error) {
    if (spec.empty()) {
        if (error) {
            *error = "empty JSON spec";
        }
        return false;
    }
    if (spec.front() == '{' || spec.front() == '[') {
        *text = spec;
        return true;
    }
    std::ifstream input(spec);
    if (!input) {
        if (error) {
            *error = "cannot open " + spec;
        }
        return false;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    *text = buffer.str();
    return true;
}

void dump_targets(const UiHitMap& hits) {
    log::info("ui targets: %zu", hits.targets().size());
    for (const UiTarget& target : hits.targets()) {
        log::info("  %-16s index=%d rect=%.0f,%.0f %.0fx%.0f", ui_action_name(target.action),
                  target.index, target.rect.x, target.rect.y, target.rect.w, target.rect.h);
    }
}

// Every screen shares the client caption's window controls. A backend with no
// client caption keeps the Display no-ops.
void add_window_handlers(UiActionHandlers* handlers, Display* display) {
    handlers->window_minimize = [display]() { display->minimize_window(); };
    handlers->window_maximize = [display]() { display->toggle_maximize_window(); };
    handlers->window_close = [display]() { display->request_close(); };
}

struct ScriptHooks {
    UiInputRouter* router = nullptr;
    UiFocus* focus = nullptr;
    UiActionHandlers handlers;
    std::function<void()> refresh;
    std::function<bool()> escape;
    std::function<bool()> dismiss;
    std::function<void()> toggle_debug;
    std::function<void(const InputEvent&)> observe;
    // Pointer events for the Puck interaction; `now` is the script or app time.
    std::function<void(const InputEvent&, float)> pointer;
    float* clock = nullptr;
};

// Runs an action the focus layer produced through the same handlers as a
// pointer click. `changed` marks the screen dirty or re-renders it.
bool apply_focus_result(const UiFocusResult& result, const UiActionHandlers& handlers,
                        const std::function<bool()>& dismiss,
                        const std::function<void()>& toggle_debug,
                        const std::function<void()>& changed) {
    if (result.dismiss) {
        if (dismiss && dismiss() && changed) {
            changed();
        }
        return true;
    }
    if (result.toggle_debug) {
        if (toggle_debug) {
            toggle_debug();
        }
        if (changed) {
            changed();
        }
        return true;
    }
    if (result.action != UiAction::None) {
        if (dispatch_ui_action(result.action, result.index, handlers) && changed) {
            changed();
        }
        return true;
    }
    return result.consumed;
}

std::string focus_label(const UiFocus& focus) {
    if (!focus.focused()) {
        return "none";
    }
    const int index = focus.focused_index();
    const std::string name = ui_action_name(focus.focused_action());
    return index >= 0 ? name + "[" + std::to_string(index) + "]" : name;
}

// Applies scripted events against the hit map from the previous render, then
// refreshes so the next event sees the new screen. Gamepad events go through
// UiFocus first; pointer events that do not start an End hold fall through to
// the pointer router unchanged.
bool apply_input_script(const std::vector<ScriptedInput>& script, const ScriptHooks& hooks) {
    if (!hooks.router) {
        return false;
    }
    for (const ScriptedInput& entry : script) {
        const float now = static_cast<float>(entry.t);
        if (hooks.clock) {
            *hooks.clock = now;
        }
        if (hooks.observe) {
            hooks.observe(entry.event);
        }
        if (hooks.pointer) {
            hooks.pointer(entry.event, now);
        }
        if (hooks.focus) {
            const std::string before = focus_label(*hooks.focus);
            const UiFocusResult hold = hooks.focus->update(now);
            if (hold.action != UiAction::None) {
                log::info("input: %s hold at t=%.2f", ui_action_name(hold.action), entry.t);
                apply_focus_result(hold, hooks.handlers, hooks.dismiss, hooks.toggle_debug,
                                   hooks.refresh);
            }
            const UiFocusResult result = hooks.focus->handle(entry.event, now);
            const bool consumed = apply_focus_result(
                result, hooks.handlers, hooks.dismiss, hooks.toggle_debug, hooks.refresh);
            const std::string after = focus_label(*hooks.focus);
            if (after != before) {
                log::info("input: focus %s at t=%.2f", after.c_str(), entry.t);
            }
            if (consumed) {
                continue;
            }
        }
        int index = -1;
        const UiAction action = hooks.router->handle(entry.event, &index);
        if (action == UiAction::Escape) {
            log::info("input: escape at t=%.2f", entry.t);
            if (hooks.escape && hooks.escape()) {
                return true;
            }
            continue;
        }
        if (action == UiAction::None) {
            continue;
        }
        const std::string label =
            index >= 0 ? std::string(ui_action_name(action)) + "[" + std::to_string(index) + "]"
                       : std::string(ui_action_name(action));
        if (!dispatch_ui_action(action, index, hooks.handlers)) {
            log::warn("input: no handler for %s", label.c_str());
            continue;
        }
        log::info("input: %s at t=%.2f", label.c_str(), entry.t);
        if (hooks.refresh) {
            hooks.refresh();
        }
    }
    return hooks.router->quit_requested();
}

int run_voice_loopback_mode(const AppOptions& options) {
    std::string error;
    FileAudioOptions first_options;
    first_options.tone_hz = 440.0;
    first_options.capture_path = "build/voice-loopback-a-capture.wav";
    first_options.playback_path = "build/voice-loopback-a.wav";
    FileAudioOptions second_options;
    second_options.tone_hz = 660.0;
    second_options.capture_path = "build/voice-loopback-b-capture.wav";
    second_options.playback_path = "build/voice-loopback-b.wav";

    auto first = create_file_audio(first_options, &error);
    if (!first) {
        log::error("loopback audio A: %s", error.c_str());
        return 1;
    }
    auto second = create_file_audio(second_options, &error);
    if (!second) {
        log::error("loopback audio B: %s", error.c_str());
        return 1;
    }

    voice::LoopbackOptions loop;
    loop.seconds = options.voice_loopback_seconds;
    loop.aec = options.voice_aec;
    loop.reorder_every = 5;
    loop.drop_every = 17;
    const int result = voice::run_voice_loopback(*first, *second, loop);

    for (const char* path : {first_options.playback_path.c_str(),
                             second_options.playback_path.c_str(),
                             first_options.capture_path.c_str(),
                             second_options.capture_path.c_str()}) {
        std::error_code code;
        const auto size = std::filesystem::file_size(path, code);
        if (!code) {
            log::info("loopback wav %s: %llu bytes", path,
                      static_cast<unsigned long long>(size));
        }
    }
    return result;
}

CallView demo_call_view(const std::string& state) {
    CallView view;
    view.input_level = 0.05f;
    view.jitter_ms = 60;
    if (state == "idle") {
        view.phase = CallPhase::Idle;
        view.caption = "Call Puck";
    } else if (state == "connecting") {
        view.phase = CallPhase::Connecting;
        view.caption = "Connecting";
        view.detail = "Setting up the voice channel";
    } else if (state == "speaking") {
        view.phase = CallPhase::Speaking;
        view.caption = "Puck is speaking";
        view.detail = "I can help with that.";
        view.speaking = true;
        view.output_level = 0.2f;
        view.mouth = 0.7f;
    } else if (state == "thinking") {
        view.phase = CallPhase::Thinking;
        view.caption = "Puck is thinking";
        view.detail = "Working on your request";
        view.thinking = true;
    } else if (state == "muted") {
        view.phase = CallPhase::Muted;
        view.caption = "Microphone off";
        view.detail = "Puck can't hear you";
        view.muted = true;
    } else if (state == "ended") {
        view.phase = CallPhase::Ended;
        view.caption = "Call Ended";
        view.detail = "The call ended";
    } else if (state == "failed") {
        view.phase = CallPhase::Failed;
        view.caption = "Call failed";
        view.detail = "WebRTC connect timeout";
    } else {
        view.phase = CallPhase::Active;
        view.caption = "On a call with Puck";
        view.detail = "Puck is listening";
    }
    return view;
}

bool load_demo_media(const std::string& spec, MediaOverlay& media, std::string* error) {
    if (spec == "null") {
        media.close();
        return true;
    }
    if (spec.empty()) {
        *error = "empty demo media";
        return false;
    }

    std::string text = spec;
    if (spec.front() != '{' && spec.front() != '[') {
        std::error_code code;
        if (std::filesystem::is_regular_file(spec, code)) {
            const std::string extension = std::filesystem::path(spec).extension().string();
            if (extension == ".json") {
                std::ifstream input(spec);
                if (!input) {
                    *error = "cannot open " + spec;
                    return false;
                }
                std::stringstream buffer;
                buffer << input.rdbuf();
                text = buffer.str();
            } else {
                nlohmann::json presentation = {
                    {"id", "demo"},
                    {"title", "Generated picture"},
                    {"items",
                     nlohmann::json::array({{{"id", "demo-1"},
                                             {"url", spec},
                                             {"caption", "Generated picture"}}})},
                };
                return media.set_presentation(presentation, true, error);
            }
        } else if (std::filesystem::path(spec).extension() == ".json") {
            *error = "cannot find " + spec;
            return false;
        } else {
            // A missing local picture is intentional: it exercises the
            // fetch-failure card without a live session.
            nlohmann::json presentation = {
                {"id", "demo"},
                {"title", "Generated picture"},
                {"items",
                 nlohmann::json::array({{{"id", "demo-1"},
                                         {"url", spec},
                                         {"caption", "Generated picture"}}})},
            };
            return media.set_presentation(presentation, true, error);
        }
    }

    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        *error = "demo media is not valid JSON";
        return false;
    }
    if (parsed.is_object() && parsed.contains("presentation")) {
        return media.set_presentation(parsed["presentation"], true, error);
    }
    return media.set_presentation(parsed, true, error);
}

int run_demo_screen(const AppOptions& options, Platform& platform, Renderer& renderer, CarUi& ui,
                    MediaOverlay& media, PluginUi& plugins, PasskeyFlow& passkey,
                    Diagnostics& diagnostics, bool media_mode) {
    CallView view = demo_call_view(media_mode ? "active" : options.demo_call_state);
    PuckVisual visual;
    visual.speaking = view.speaking;
    visual.listening = view.phase == CallPhase::Active && !view.muted;
    visual.thinking = view.thinking;
    visual.not_listening = view.muted;
    visual.mouth = view.mouth;

    UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    float demo_time = 0.0f;
    PuckInteraction puck_interaction;

    if (!options.demo_plugin.empty()) {
        std::string plugin_text;
        std::string error;
        if (!load_text_spec(options.demo_plugin, &plugin_text, &error)) {
            log::error("demo plugin: %s", error.c_str());
            return 2;
        }
        plugins.on_actor_message(plugin_text);
    }
    plugins.set_sender([&](const std::string& text) {
        log::info("plugin ui -> %s", redact_json(text, 300).c_str());
        return true;
    });

    if (options.demo_passkey == "phone") {
        view = demo_call_view("idle");
        passkey.set_context(PasskeyContext{true, true, false, true});
    } else if (options.demo_passkey == "local" || options.demo_passkey == "confirming") {
        view = demo_call_view("idle");
        passkey.set_context(PasskeyContext{false, true, false, true});
        if (options.demo_passkey == "confirming") {
            passkey.confirm();
        }
    }

    diagnostics.set_session(false, {});
    if (options.demo_debug) {
        diagnostics.open();
    }

    auto sync = [&]() {
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        puck_interaction.update(demo_time);
        PuckVisual posed = visual;
        posed.pose = puck_interaction.pose();
        ui.set_visual(posed);
        ui.set_call(view);
        ui.set_sheet(plugins.sheet());
        ui.set_passkey(passkey.view());
        ui.set_debug(diagnostics.view());
    };
    auto render = [&](bool present) {
        platform.display->make_current();
        media.poll();
        focus.sync();
        ui.set_focus(focus.view(demo_time, ui.hints_visible()));
        renderer.begin(platform.display->size(), platform.display->dpi_scale());
        ui.draw(renderer.canvas(), platform.display->size(), demo_time);
        renderer.end();
        focus.sync();
        if (present) {
            platform.display->swap();
        }
    };

    sync();
    render(false);
    if (options.dump_targets) {
        dump_targets(ui.hit_map());
    }
    // The first sync ran before the layout existed; the render just filled in
    // the Puck rect, so hand it to the interaction before scripted input.
    puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());

    UiActionHandlers handlers;
    handlers.call = [&]() {
        view = demo_call_view("active");
        visual.listening = true;
    };
    handlers.mute = [&]() {
        view.muted = !view.muted;
        view.phase = view.muted ? CallPhase::Muted : CallPhase::Active;
        view.caption = view.muted ? "Microphone off" : "On a call with Puck";
        view.detail = view.muted ? "Puck can't hear you" : "Puck is listening";
        visual.not_listening = view.muted;
        visual.listening = !view.muted;
    };
    handlers.end = [&]() {
        view.phase = CallPhase::Ended;
        view.caption = "Call Ended";
        view.detail = "The call ended";
        view.muted = false;
        visual = PuckVisual{};
    };
    handlers.media_close = [&]() { media.close(); };
    handlers.media_previous = [&]() { media.previous(); };
    handlers.media_next = [&]() { media.next(); };
    handlers.sheet_option = [&](int index) {
        if (plugins.sheet().kind == PluginSheetKind::Confirm) {
            plugins.confirm(index == 0);
        } else {
            plugins.select(index);
        }
    };
    handlers.sheet_dismiss = [&]() { plugins.dismiss(); };
    handlers.sheet_ok = [&]() { plugins.phone_ok(); };
    handlers.passkey_confirm = [&]() { passkey.confirm(); };
    handlers.debug_open = [&]() { diagnostics.open(); };
    handlers.debug_close = [&]() { diagnostics.close(); };
    handlers.echo_mode = [&](int index) {
        diagnostics.set_echo_mode(index == 0   ? EchoCancellationMode::On
                                  : index == 1 ? EchoCancellationMode::RemoteOnly
                                               : EchoCancellationMode::Off);
    };
    handlers.mic_test = [&]() {
        std::string error;
        diagnostics.run_mic_test(platform.audio.get(), &error);
    };
    handlers.send_diagnostics = [&]() {
        std::string error;
        diagnostics.send(&error);
    };
    add_window_handlers(&handlers, platform.display.get());

    UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    auto dismiss_overlay = [&]() -> bool {
        if (diagnostics.is_open()) {
            diagnostics.close();
            return true;
        }
        if (plugins.visible()) {
            plugins.dismiss();
            return true;
        }
        if (media.visible()) {
            media.close();
            return true;
        }
        return false;
    };
    auto toggle_debug = [&]() {
        if (diagnostics.is_open()) {
            diagnostics.close();
        } else {
            diagnostics.open();
        }
    };
    auto puck_input_allowed = [&]() {
        return !diagnostics.is_open() && !plugins.visible() && !media.visible() &&
               passkey.view().mode == PasskeyMode::Hidden;
    };
    ScriptHooks hooks;
    hooks.router = &router;
    hooks.focus = &focus;
    hooks.handlers = handlers;
    hooks.refresh = [&]() {
        sync();
        render(false);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
    };
    hooks.escape = [&]() {
        if (dismiss_overlay()) {
            sync();
            render(false);
            return false;
        }
        return true;
    };
    hooks.dismiss = dismiss_overlay;
    hooks.toggle_debug = toggle_debug;
    hooks.observe = [&](const InputEvent& event) { ui.notify_input(event); };
    hooks.pointer = [&](const InputEvent& event, float now) {
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        if (event.type == InputEvent::Type::PointerDown) {
            if (!ui.hit_map().hit(event.x, event.y) && puck_input_allowed()) {
                puck_interaction.pointer_down(event.x, event.y, now);
            }
        } else if (event.type == InputEvent::Type::PointerMove) {
            puck_interaction.pointer_move(event.x, event.y, now);
        } else if (event.type == InputEvent::Type::PointerUp) {
            puck_interaction.pointer_up(event.x, event.y, now);
        }
    };
    hooks.clock = &demo_time;
    apply_input_script(options.input_script, hooks);

    sync();
    render(true);
    log::info("demo: call=%s media=%s focus=%s", view.caption.c_str(),
              media.status_text().c_str(), focus_label(focus).c_str());
    return 0;
}

int run_demo_media(const AppOptions& options, Platform& platform, Renderer& renderer, CarUi& ui,
                   MediaOverlay& media, PluginUi& plugins, PasskeyFlow& passkey,
                   Diagnostics& diagnostics) {
    std::string error;
    if (!load_demo_media(options.demo_media, media, &error)) {
        log::error("media demo: %s", error.c_str());
        return 2;
    }
    media.set_index(options.demo_media_index);

    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!media.settled() && Clock::now() < deadline) {
        platform.display->make_current();
        media.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!media.settled()) {
        log::warn("media demo: still loading after 10 seconds");
    }
    return run_demo_screen(options, platform, renderer, ui, media, plugins, passkey, diagnostics,
                           true);
}

int run_pairing(const AppOptions& options, Platform& platform, Renderer& renderer, CarUi& ui,
                MediaOverlay& media, PluginUi& plugins, PasskeyFlow& passkey,
                Diagnostics& diagnostics) {
    if (!platform.http) {
        log::error("pairing requires an HTTP client");
        return 1;
    }
    if (!platform.display) {
        log::error("pairing requires a display");
        return 1;
    }

    diagnostics.set_environment(platform.display->size(), platform.display->dpi_scale(),
                                platform.display->backend_name(),
                                std::string(platform.display->backend_name()) != "headless");
    diagnostics.set_audio(platform.audio ? platform.audio->name() : "none", platform.audio != nullptr);
    diagnostics.set_session(false, {});

    PairingSession pairing(*platform.http, platform.secure_store.get());
    CarSession saved;
    std::string error;
    if (pairing.restore(&saved, &error)) {
        if (!saved.thread_id.empty()) {
            log::info("restored saved session for thread %s", saved.thread_id.c_str());
        } else {
            log::info("restored saved cookies");
        }
    } else {
        log::info("%s", error.c_str());
    }

    CarSession session;
    bool resumed = false;
    if (!saved.thread_id.empty()) {
        if (pairing.load_car_session(&session, &error) && !session.thread_id.empty()) {
            log::info("saved session still valid; skipping pairing");
            resumed = true;
        } else {
            log::info("saved session not reusable (%s); pairing again",
                      redact_text(error).c_str());
        }
    }

    if (!resumed && !pairing.start(&error)) {
        log::error("cannot start pairing: %s", error.c_str());
        return 1;
    }

    const auto started = Clock::now();
    auto elapsed_seconds = [&]() {
        return std::chrono::duration<float>(Clock::now() - started).count();
    };
    UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    float script_time = 0.0f;
    bool use_script_time = false;
    PuckInteraction puck_interaction;
    auto ui_time = [&]() { return use_script_time ? script_time : elapsed_seconds(); };
    auto render = [&](bool present) {
        platform.display->make_current();
        media.poll();
        focus.sync();
        ui.set_focus(focus.view(ui_time(), ui.hints_visible()));
        renderer.begin(platform.display->size(), platform.display->dpi_scale());
        ui.draw(renderer.canvas(), platform.display->size(), ui_time());
        renderer.end();
        focus.sync();
        if (present) {
            platform.display->swap();
        }
    };

    UiInputRouter router;
    router.set_hit_map(&ui.hit_map());

    bool dirty = true;
    bool quit = false;
    UiActionHandlers debug_handlers;
    debug_handlers.debug_open = [&]() { diagnostics.open(); };
    debug_handlers.debug_close = [&]() { diagnostics.close(); };
    debug_handlers.echo_mode = [&](int index) {
        diagnostics.set_echo_mode(index == 0   ? EchoCancellationMode::On
                                  : index == 1 ? EchoCancellationMode::RemoteOnly
                                               : EchoCancellationMode::Off);
    };
    debug_handlers.mic_test = [&]() {
        std::string mic_error;
        diagnostics.run_mic_test(platform.audio.get(), &mic_error);
    };
    debug_handlers.send_diagnostics = [&]() {
        std::string send_error;
        diagnostics.send(&send_error);
    };
    add_window_handlers(&debug_handlers, platform.display.get());

    auto sync_pairing = [&]() {
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        puck_interaction.update(ui_time());
        PuckVisual posed;
        posed.pose = puck_interaction.pose();
        ui.set_visual(posed);
        ui.set_sheet(PluginSheet{});
        ui.set_passkey(PasskeyView{});
        ui.set_debug(diagnostics.view());
    };
    auto dismiss_overlay = [&]() -> bool {
        if (diagnostics.is_open()) {
            diagnostics.close();
            return true;
        }
        return false;
    };
    auto toggle_debug = [&]() {
        if (diagnostics.is_open()) {
            diagnostics.close();
        } else {
            diagnostics.open();
        }
    };
    auto puck_input_allowed = [&]() {
        return !diagnostics.is_open() && !plugins.visible() && !media.visible() &&
               passkey.view().mode == PasskeyMode::Hidden;
    };
    auto handle_puck_pointer = [&](const InputEvent& event, float now) {
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        if (event.type == InputEvent::Type::PointerDown) {
            if (!ui.hit_map().hit(event.x, event.y) && puck_input_allowed()) {
                puck_interaction.pointer_down(event.x, event.y, now);
            }
        } else if (event.type == InputEvent::Type::PointerMove) {
            puck_interaction.pointer_move(event.x, event.y, now);
        } else if (event.type == InputEvent::Type::PointerUp) {
            puck_interaction.pointer_up(event.x, event.y, now);
        }
    };
    auto poll_debug_input = [&]() {
        const UiFocusResult hold = focus.update(elapsed_seconds());
        if (hold.action != UiAction::None) {
            apply_focus_result(hold, debug_handlers, dismiss_overlay, toggle_debug,
                               [&]() { dirty = true; });
        }
        InputEvent event;
        while (platform.input && platform.input->poll(event)) {
            const bool hints_before = ui.hints_visible();
            ui.notify_input(event);
            if (hints_before != ui.hints_visible()) {
                dirty = true;
            }
            const float now = elapsed_seconds();
            handle_puck_pointer(event, now);
            const std::string before = focus_label(focus);
            const UiFocusResult focus_result = focus.handle(event, now);
            const bool consumed = apply_focus_result(
                focus_result, debug_handlers, dismiss_overlay, toggle_debug,
                [&]() { dirty = true; });
            const std::string after = focus_label(focus);
            if (after != before) {
                log::info("pairing: focus %s", after.c_str());
                dirty = true;
            }
            if (consumed) {
                continue;
            }

            int index = -1;
            const UiAction action = router.handle(event, &index);
            if (action == UiAction::Escape) {
                if (diagnostics.is_open()) {
                    diagnostics.close();
                    dirty = true;
                } else {
                    log::info("pairing: escape; quitting");
                    quit = true;
                }
                continue;
            }
            if (dispatch_ui_action(action, index, debug_handlers)) {
                dirty = true;
            }
        }
        if (router.quit_requested()) {
            quit = true;
        }
    };

    auto show_challenge = [&]() {
        ui.set_pairing(pairing.qr(), "Call Puck", "Waiting for scan");
        sync_pairing();
        render(true);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
        std::cout << pairing.pairing_url() << std::endl;
        log::info("pairing QR written to %s", options.platform.output_path.c_str());
    };

    if (!resumed) {
        show_challenge();
    } else {
        ui.set_pairing(pairing.qr(), "Connecting", "Restoring the saved session");
        sync_pairing();
        render(true);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
    }
    if (!options.input_script.empty()) {
        use_script_time = true;
        ScriptHooks hooks;
        hooks.router = &router;
        hooks.focus = &focus;
        hooks.handlers = debug_handlers;
        hooks.refresh = [&]() {
            sync_pairing();
            render(true);
            if (options.dump_targets) {
                dump_targets(ui.hit_map());
            }
        };
        hooks.escape = [&]() {
            if (dismiss_overlay()) {
                sync_pairing();
                render(true);
                return false;
            }
            return true;
        };
        hooks.dismiss = dismiss_overlay;
        hooks.toggle_debug = toggle_debug;
        hooks.observe = [&](const InputEvent& event) { ui.notify_input(event); };
        hooks.pointer = handle_puck_pointer;
        hooks.clock = &script_time;
        if (apply_input_script(options.input_script, hooks)) {
            return 0;
        }
        if (options.exit_after_script) {
            sync_pairing();
            render(true);
            if (options.dump_targets) {
                dump_targets(ui.hit_map());
            }
            log::info("input script finished; leaving the pairing screen");
            return 0;
        }
        use_script_time = false;
    }

    int generation = pairing.challenge_generation();
    std::string shown_code;

    const auto deadline = Clock::now() + std::chrono::seconds(options.pair_timeout_seconds);
    PairingStatus status;
    if (resumed) {
        status.state = PairingState::Paired;
    }
    PairingState last_state = PairingState::Idle;
    auto next_poll = Clock::now();
    auto last_render = Clock::now();
    while (!resumed && !quit && Clock::now() < deadline) {
        poll_debug_input();

        if (Clock::now() >= next_poll) {
            next_poll = Clock::now() + std::chrono::seconds(pairing.poll_interval_seconds());
            status = pairing.poll(&error);
            if (!error.empty()) {
                log::warn("pairing poll failed: %s", error.c_str());
                error.clear();
            }
            if (status.state != last_state) {
                last_state = status.state;
                log::info("pairing state: %s", status.message.c_str());
            }
            if (pairing.challenge_generation() != generation) {
                generation = pairing.challenge_generation();
                shown_code.clear();
                log::info("pairing challenge changed; scan the new QR");
                show_challenge();
                dirty = false;
            }
            if (status.state == PairingState::Scanned && !status.code.empty() &&
                status.code != shown_code) {
                shown_code = status.code;
                ui.set_pairing(pairing.qr(), "Confirm on Your Device", "Confirm on your device",
                               status.code);
                sync_pairing();
                render(true);
                dirty = false;
                log::info("pairing scanned; code %s", status.code.c_str());
            }
            if (status.state == PairingState::Paired) {
                break;
            }
            if (status.state == PairingState::Failed || status.state == PairingState::Expired) {
                log::error("pairing failed: %s", status.message.c_str());
                return 1;
            }
        }

        if (puck_interaction.active()) {
            dirty = true;
        }
        // Headless has no input and writes a PPM per frame, so only re-render
        // while the debug panel is open there.
        const bool headless = std::string(platform.display->backend_name()) == "headless";
        const bool desktop = std::string(platform.display->backend_name()) == "win32";
        if (dirty || (diagnostics.is_open() && !headless &&
                      Clock::now() - last_render >= std::chrono::milliseconds(100)) ||
            (desktop && Clock::now() - last_render >= std::chrono::milliseconds(16))) {
            sync_pairing();
            render(true);
            last_render = Clock::now();
            dirty = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (quit) {
        return 0;
    }
    if (status.state != PairingState::Paired) {
        log::error("pairing did not complete within %d seconds", options.pair_timeout_seconds);
        return 1;
    }

    ui.set_pairing(pairing.qr(), "Connecting", "Paired. Starting session...");
    sync_pairing();
    render(true);

    if (!resumed) {
        if (!pairing.establish_session(&error)) {
            log::error("car session: %s", error.c_str());
            return 1;
        }

        if (!pairing.load_car_session(&session, &error)) {
            log::error("car data: %s", error.c_str());
            return 1;
        }

        if (pairing.persist(session, &error)) {
            log::info("car session saved for thread %s", session.thread_id.c_str());
        } else {
            log::warn("cannot persist car session: %s", error.c_str());
        }
    }

    log::info("car session ready: thread=%s actor=%s capability=%s", session.thread_id.c_str(),
              redact_url(session.actor_base_url).c_str(),
              session.capability.empty() ? "default" : session.capability.c_str());

    diagnostics.set_session(true, session.thread_id);
    if (platform.http) {
        std::string usage_error;
        const bool coarse = std::string(platform.display->backend_name()) != "headless";
        if (post_car_usage(*platform.http, pairing.origin(), "direct", coarse,
                           platform.display->size().width, platform.display->size().height,
                           &usage_error)) {
            log::info("car usage: posted entry=direct");
        } else {
            log::warn("car usage: %s", redact_text(usage_error).c_str());
        }
    }

    if (!platform.websockets) {
        log::error("WebSocket factory is unavailable");
        return 2;
    }

    std::string token_error;
    if (!pairing.fetch_actor_token(&session, &token_error)) {
        log::warn("thread actor token: %s (trying the gateway token)",
                  redact_text(token_error).c_str());
    }

    const std::string page_load_id = random_uuid();
    std::string ready_error;
    if (wait_actor_ready(*platform.http, session, page_load_id, &ready_error)) {
        log::info("actor: ready");
    } else {
        log::warn("actor ready check: %s", redact_text(ready_error).c_str());
    }

    const std::vector<HttpHeader> headers = {
        {"Origin", pairing.origin()},
        {"User-Agent", "puck-kiosk/0.1"},
    };
    int resume_id = 0;
    auto connect_actor = [&](std::unique_ptr<WebSocket>* out) {
        ActorEndpoint endpoint;
        if (!build_actor_endpoint(session, page_load_id, &endpoint, &error)) {
            log::error("actor endpoint: %s", error.c_str());
            return false;
        }
        log::info("actor: connecting to %s", endpoint.display_url.c_str());
        auto socket =
            platform.websockets->connect(endpoint.url, headers, endpoint.subprotocols, &error);
        if (!socket) {
            log::error("actor connect: %s", redact_text(error).c_str());
            return false;
        }
        // The actor only starts pushing after the client resumes its observer
        // session; version 0 asks for the full snapshot.
        const std::string resume = nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", "puck-kiosk-" + std::to_string(resume_id++)},
            {"method", "client_resume"},
            {"params", {{"version", 0}}},
        }.dump();
        socket->send_text(resume);
        *out = std::move(socket);
        return true;
    };

    std::unique_ptr<WebSocket> websocket;
    if (!connect_actor(&websocket)) {
        return 2;
    }
    double token_exp = ws_token_exp_seconds(session.ws_token);

    ActorReader reader;
    reader.start(websocket.get());
    plugins.set_sender([&](const std::string& text) {
        return websocket && websocket->is_open() && websocket->send_text(text);
    });

    log::info("actor: connected; observing for %d seconds", options.actor_seconds);

    std::unique_ptr<voice::VoiceEngine> voice_engine;
    std::unique_ptr<CallController> call;
    voice::VoiceEngineStats final_stats;
    CallView ended_view;
    std::string call_error_text;
    bool call_failed = false;
    bool ended_by_user = false;
    auto start_call = [&]() -> bool {
        if (!platform.audio) {
            log::error("voice: no audio backend is available");
            return false;
        }
        if (call) {
            call->end("restarting the call");
            call.reset();
        }
        voice_engine.reset();
        ended_view = CallView{};
        call_error_text.clear();
        call_failed = false;
        voice::VoiceEngineOptions voice_options;
        voice_options.enable_aec = diagnostics.echo_mode() == EchoCancellationMode::On;
        voice_engine = std::make_unique<voice::VoiceEngine>(*platform.audio, voice_options);
        CallOptions call_options;
        call_options.mute_after_seconds = options.mute_after_seconds;
        call_options.end_after_seconds = options.end_after_seconds;
        call = std::make_unique<CallController>(*websocket, *voice_engine, call_options);
        voice_engine->set_data_callback([&call](const std::string& text) {
            if (call) {
                call->on_data_message(text);
            }
        });
        std::string call_error;
        if (!call->start(&call_error)) {
            log::error("voice: cannot start call: %s", call_error.c_str());
            call.reset();
            voice_engine.reset();
            return false;
        }
        return true;
    };

    if (options.call && !start_call()) {
        return 2;
    }

    int message_count = 0;
    int passkey_request_id = 0;
    int next_request_id = 1000;
    auto last_ping = Clock::now();
    auto last_car_render = Clock::now();
    auto last_renew = Clock::now();
    const auto actor_deadline = Clock::now() + std::chrono::seconds(options.actor_seconds);
    const bool observe_forever = options.actor_seconds == 0;

    auto update_passkey_context = [&]() {
        PasskeyContext context;
        context.car_session = true;
        context.actor_connected = websocket && websocket->is_open();
        context.can_write = session.capability.empty() || session.capability == "write";
        context.requires_sudo = session.requires_sudo_for_write;
        passkey.set_context(context);
    };

    auto sync_car = [&]() {
        PuckVisual visual;
        if (call) {
            visual = call->visual();
        }
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        puck_interaction.update(ui_time());
        visual.pose = puck_interaction.pose();
        if (call) {
            ui.set_call(call->view());
        } else if (ended_view.phase == CallPhase::Ended ||
                   ended_view.phase == CallPhase::Failed) {
            ui.set_call(ended_view);
        } else {
            ui.set_call(CallView{});
        }
        ui.set_visual(visual);
        ui.set_sheet(plugins.sheet());
        ui.set_passkey(passkey.view());
        ui.set_debug(diagnostics.view());
        diagnostics.set_call_active(call && call->active());
    };

    UiActionHandlers handlers;
    handlers.call = [&]() { start_call(); };
    handlers.mute = [&]() {
        if (call) {
            call->mute(!call->view().muted);
        }
    };
    handlers.end = [&]() {
        if (call) {
            ended_by_user = true;
            call->end("ended from the car screen");
        }
    };
    handlers.media_close = [&]() { media.close(); };
    handlers.media_previous = [&]() { media.previous(); };
    handlers.media_next = [&]() { media.next(); };
    handlers.sheet_option = [&](int index) {
        if (plugins.sheet().kind == PluginSheetKind::Confirm) {
            plugins.confirm(index == 0);
        } else {
            plugins.select(index);
        }
    };
    handlers.sheet_dismiss = [&]() { plugins.dismiss(); };
    handlers.sheet_ok = [&]() { plugins.phone_ok(); };
    handlers.passkey_confirm = [&]() {
        if (!websocket || !websocket->is_open()) {
            return;
        }
        if (session.ws_token.empty()) {
            log::warn("passkey: no actor token to re-authorize");
            return;
        }
        if (!passkey.confirm()) {
            return;
        }
        // The web has no passkey JSON-RPC; its local button runs WebAuthn and
        // the sudo HTTP endpoints. A kiosk without an authenticator can only
        // ask the actor to re-authorize this connection, so send the one real
        // authorization request the actor protocol exposes.
        passkey_request_id = next_request_id++;
        const nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", passkey_request_id},
            {"method", "client_reauthorize"},
            {"params", {{"wsToken", session.ws_token}}},
        };
        if (!websocket->send_text(request.dump())) {
            passkey.on_request_result(false, "Cannot reach the actor");
            passkey_request_id = 0;
        }
    };
    handlers.debug_open = [&]() { diagnostics.open(); };
    handlers.debug_close = [&]() { diagnostics.close(); };
    handlers.echo_mode = [&](int index) {
        diagnostics.set_echo_mode(index == 0   ? EchoCancellationMode::On
                                  : index == 1 ? EchoCancellationMode::RemoteOnly
                                               : EchoCancellationMode::Off);
    };
    handlers.mic_test = [&]() {
        std::string mic_error;
        diagnostics.run_mic_test(platform.audio.get(), &mic_error);
    };
    handlers.send_diagnostics = [&]() {
        std::string send_error;
        diagnostics.send(&send_error);
    };
    add_window_handlers(&handlers, platform.display.get());

    auto dismiss_car_overlay = [&]() -> bool {
        if (diagnostics.is_open()) {
            diagnostics.close();
            return true;
        }
        if (plugins.visible()) {
            plugins.dismiss();
            return true;
        }
        if (media.visible()) {
            media.close();
            return true;
        }
        return false;
    };
    auto toggle_car_debug = [&]() {
        if (diagnostics.is_open()) {
            diagnostics.close();
        } else {
            diagnostics.open();
        }
    };

    // Pairing left focus on its Debug entry; the call screen starts fresh.
    focus.reset();

    auto handle_actor_message = [&](const std::string& message) {
        if (message == "pong") {
            return;
        }
        ++message_count;
        log_actor_message(message);
        if (call) {
            call->on_actor_message(message);
        }
        media.on_actor_message(message);
        plugins.on_actor_message(message);
        if (passkey_request_id != 0) {
            const nlohmann::json parsed = nlohmann::json::parse(message, nullptr, false);
            auto check = [&](const nlohmann::json& value) {
                if (!value.is_object() || !value.contains("id") ||
                    (!value.contains("result") && !value.contains("error"))) {
                    return false;
                }
                bool matches = false;
                if (value["id"].is_number_integer()) {
                    matches = value["id"].get<int>() == passkey_request_id;
                } else if (value["id"].is_string()) {
                    matches = value["id"].get<std::string>() == std::to_string(passkey_request_id);
                }
                if (!matches) {
                    return false;
                }
                if (value.contains("error")) {
                    passkey.on_request_result(false, redact_json(value["error"].dump(), 200));
                } else {
                    passkey.on_request_result(true, {});
                }
                passkey_request_id = 0;
                return true;
            };
            if (parsed.is_array()) {
                for (const nlohmann::json& entry : parsed) {
                    if (check(entry)) {
                        break;
                    }
                }
            } else {
                check(parsed);
            }
        }
    };

    if (!options.input_script.empty()) {
        use_script_time = true;
        update_passkey_context();
        sync_car();
        render(false);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
        ScriptHooks hooks;
        hooks.router = &router;
        hooks.focus = &focus;
        hooks.handlers = handlers;
        hooks.refresh = [&]() {
            update_passkey_context();
            sync_car();
            render(false);
            if (options.dump_targets) {
                dump_targets(ui.hit_map());
            }
        };
        hooks.escape = [&]() {
            if (dismiss_car_overlay()) {
                sync_car();
                render(false);
                return false;
            }
            return true;
        };
        hooks.dismiss = dismiss_car_overlay;
        hooks.toggle_debug = toggle_car_debug;
        hooks.observe = [&](const InputEvent& event) { ui.notify_input(event); };
        hooks.pointer = handle_puck_pointer;
        hooks.clock = &script_time;
        if (apply_input_script(options.input_script, hooks)) {
            quit = true;
        }
        update_passkey_context();
        sync_car();
        render(true);
        if (options.exit_after_script) {
            log::info("input script finished; leaving the car screen");
            if (call) {
                final_stats = voice_engine->stats();
                call->end("input script finished");
            }
            reader.stop();
            return 0;
        }
        use_script_time = false;
    }

    if (options.input_script.empty()) {
        // Fill in the car layout before the first input poll so the Puck
        // interaction does not use the pairing screen's rect.
        update_passkey_context();
        sync_car();
        render(false);
        sync_car();
    }

    bool running = !quit;
    bool car_dirty = true;
    while (running &&
           (observe_forever || Clock::now() < actor_deadline || (call && call->active()))) {
        std::string message;
        std::string poll_error;
        while (reader.take(&message, &poll_error)) {
            handle_actor_message(message);
            car_dirty = true;
        }
        if (!poll_error.empty()) {
            log::warn("actor: %s", redact_text(poll_error).c_str());
        }
        if (reader.drained_and_closed()) {
            break;
        }

        const UiFocusResult hold = focus.update(elapsed_seconds());
        if (hold.action != UiAction::None) {
            apply_focus_result(hold, handlers, dismiss_car_overlay, toggle_car_debug,
                               [&]() { car_dirty = true; });
        }

        InputEvent event;
        while (platform.input && platform.input->poll(event)) {
            const bool hints_before = ui.hints_visible();
            ui.notify_input(event);
            if (hints_before != ui.hints_visible()) {
                car_dirty = true;
            }
            const float now = elapsed_seconds();
            handle_puck_pointer(event, now);
            const std::string before = focus_label(focus);
            const UiFocusResult focus_result = focus.handle(event, now);
            const bool consumed = apply_focus_result(
                focus_result, handlers, dismiss_car_overlay, toggle_car_debug,
                [&]() { car_dirty = true; });
            const std::string after = focus_label(focus);
            if (after != before) {
                log::info("car: focus %s", after.c_str());
                car_dirty = true;
            }
            if (consumed) {
                car_dirty = true;
                continue;
            }

            int index = -1;
            const UiAction action = router.handle(event, &index);
            if (action == UiAction::Escape) {
                if (diagnostics.is_open()) {
                    diagnostics.close();
                } else if (plugins.visible()) {
                    plugins.dismiss();
                } else if (media.visible()) {
                    media.close();
                } else {
                    log::info("car: escape; quitting");
                    running = false;
                }
                continue;
            }
            if (dispatch_ui_action(action, index, handlers)) {
                car_dirty = true;
            }
        }
        if (router.quit_requested()) {
            running = false;
        }
        if (!running) {
            break;
        }

        if (Clock::now() - last_renew >= std::chrono::seconds(30) && !media.busy()) {
            last_renew = Clock::now();
            std::string renew_error;
            if (!pairing.establish_session(&renew_error)) {
                log::warn("car session renew: %s", redact_text(renew_error).c_str());
                if (call && renew_error.find("401") != std::string::npos) {
                    call->end("car session expired");
                }
            } else {
                // The session cookie rotates on every renew; keep the stored
                // copy current or the next process resumes with a dead cookie.
                std::string persist_error;
                if (!pairing.persist(session, &persist_error)) {
                    log::warn("cannot persist renewed session: %s",
                              redact_text(persist_error).c_str());
                }
            }
        }
        const double now = std::chrono::duration<double>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        if (token_exp > 0.0 && now >= token_exp - 45.0 && !media.busy()) {
            std::string refresh_error;
            if (pairing.fetch_actor_token(&session, &refresh_error)) {
                token_exp = ws_token_exp_seconds(session.ws_token);
                reader.stop();
                websocket->close();
                if (!connect_actor(&websocket)) {
                    break;
                }
                reader.start(websocket.get());
                if (call) {
                    call->rebind_actor(*websocket);
                    call->reattach();
                }
                last_ping = Clock::now();
                log::info("actor: reconnected with a refreshed token");
            } else {
                log::warn("actor token refresh: %s", redact_text(refresh_error).c_str());
                token_exp = 0.0;
            }
        }
        if (websocket->is_open() && Clock::now() - last_ping >= std::chrono::seconds(30)) {
            websocket->send_text("ping");
            last_ping = Clock::now();
        }

        if (call) {
            // Capture counters before tick(): ending the call releases the peer.
            final_stats = voice_engine->stats();
            call->tick(Clock::now());
            if (call->ended() || call->failed()) {
                const bool failed_now = call->failed();
                ended_view = call->view();
                call_failed = failed_now;
                call_error_text = call->last_error();
                media.close();
                call.reset();
                voice_engine.reset();
                // A call ended from the car screen leaves the car UI running;
                // a scheduled or remote end keeps the observation loop's
                // existing exit behaviour.
                if (failed_now || !ended_by_user) {
                    sync_car();
                    render(true);
                    break;
                }
                ended_by_user = false;
                car_dirty = true;
            }
        }
        update_passkey_context();

        if (focus.hold_active() &&
            Clock::now() - last_car_render >= std::chrono::milliseconds(33)) {
            car_dirty = true;
        }
        if (puck_interaction.active() &&
            Clock::now() - last_car_render >= std::chrono::milliseconds(16)) {
            car_dirty = true;
        }

        const bool headless = std::string(platform.display->backend_name()) == "headless";
        // Desktop GPUs animate at 60 fps; embedded panels keep the 10 fps
        // idle cadence to save power and CPU.
        const bool desktop = std::string(platform.display->backend_name()) == "win32";
        const auto interval = headless ? std::chrono::milliseconds(0)
                                       : (desktop ? std::chrono::milliseconds(16)
                                                  : std::chrono::milliseconds(100));
        if (car_dirty || (interval.count() > 0 && Clock::now() - last_car_render >= interval)) {
            sync_car();
            render(true);
            last_car_render = Clock::now();
            car_dirty = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    media.close();
    if (call && !call->ended() && !call->failed()) {
        final_stats = voice_engine->stats();
        call->end("actor observation finished");
    }
    if (call_failed) {
        log::error("voice: call failed: %s", call_error_text.c_str());
    }
    if (voice_engine) {
        log::info(
            "voice stats: sent=%llu received=%llu decoded=%llu concealed=%llu reordered=%llu "
            "late=%llu nack=%llu retransmitted=%llu jitter=%dms in=%.3f out=%.3f",
            static_cast<unsigned long long>(final_stats.sent_packets),
            static_cast<unsigned long long>(final_stats.received_packets),
            static_cast<unsigned long long>(final_stats.decoded_frames),
            static_cast<unsigned long long>(final_stats.concealed_frames),
            static_cast<unsigned long long>(final_stats.reordered_packets),
            static_cast<unsigned long long>(final_stats.late_packets),
            static_cast<unsigned long long>(final_stats.nack_requests),
            static_cast<unsigned long long>(final_stats.retransmitted_packets),
            final_stats.jitter_target_frames * 20, final_stats.input_level,
            final_stats.output_level);
    }

    const bool actor_was_open = websocket->is_open();
    const std::string close_reason = websocket->close_reason();
    reader.stop();
    websocket->close();
    if (!close_reason.empty()) {
        log::info("actor: closed (%s)", redact_text(close_reason).c_str());
    }
    log::info("actor: observed %d message(s)", message_count);
    if (call_failed) {
        return 2;
    }
    return actor_was_open ? 0 : 2;
}

}  // namespace

int App::run(const AppOptions& options) {
    if (options.voice_aec_smoke) {
        double erle_db = 0.0;
        std::string error;
        if (!voice::run_aec_smoke_test(5, &erle_db, &error)) {
            log::error("AEC smoke failed: %s", error.c_str());
            return 1;
        }
        log::info("AEC smoke: ERLE %.1f dB", erle_db);
        return 0;
    }
    if (options.voice_loopback) {
        return run_voice_loopback_mode(options);
    }
    if (options.call && !options.pair) {
        log::error("--call requires --pair");
        return 2;
    }

    std::string error;
    PlatformOptions platform_options = options.platform;
    if (options.pair || options.demo_call || !options.demo_media.empty()) {
        // Pairing writes a snapshot after each state change, and the demos are
        // a single frame; a larger counter would suppress those writes.
        platform_options.frames = 1;
    }
    auto platform = create_platform(platform_options, &error);
    if (!platform) {
        log::error("%s", error.c_str());
        return 1;
    }
    if (platform->http) {
        platform->http = std::make_unique<SerializedHttpClient>(std::move(platform->http));
    }

    if (!platform->display->initialize(&error)) {
        log::error("%s: %s", platform->display->backend_name(), error.c_str());
        return 1;
    }
    platform->display->make_current();

    Renderer renderer;
    if (!renderer.initialize(options.font_path, &error)) {
        log::error("%s", error.c_str());
        return 1;
    }

    auto puck = create_puck_view(options.enable_3d);
    log::info("Puck renderer: %s", puck->name());

    CarUi ui(*puck);
    if (platform->display->needs_client_caption()) {
        ui.set_client_caption(true);
        platform->display->set_client_caption(
            ClientCaption{CarUi::kCaptionHeight, CarUi::kCaptionButtonsWidth});
    }
    MediaOverlay media(platform->http.get(), PairingOptions{}.origin, renderer.canvas());
    ui.set_media(&media);

    PluginUi plugins;
    PasskeyFlow passkey;
    Diagnostics diagnostics(platform->http.get(), PairingOptions{}.origin);
    diagnostics.set_environment(platform->display->size(), platform->display->dpi_scale(),
                                platform->display->backend_name(),
                                std::string(platform->display->backend_name()) != "headless");
    diagnostics.set_audio(platform->audio ? platform->audio->name() : "none",
                          platform->audio != nullptr);
    EchoCancellationMode echo_mode = EchoCancellationMode::On;
    if (!options.voice_aec) {
        echo_mode = EchoCancellationMode::Off;
    } else if (!parse_echo_cancellation(options.echo_cancellation, &echo_mode)) {
        log::error("unknown echo cancellation mode: %s", options.echo_cancellation.c_str());
        return 2;
    }
    diagnostics.set_echo_mode(echo_mode);

    if (!options.demo_media.empty()) {
        return run_demo_media(options, *platform, renderer, ui, media, plugins, passkey,
                              diagnostics);
    }
    if (options.demo_call) {
        return run_demo_screen(options, *platform, renderer, ui, media, plugins, passkey,
                               diagnostics, false);
    }
    if (options.pair) {
        return run_pairing(options, *platform, renderer, ui, media, plugins, passkey, diagnostics);
    }

    const auto started = std::chrono::steady_clock::now();
    bool running = true;
    UiInputRouter router;
    router.set_hit_map(&ui.hit_map());
    UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    PuckInteraction puck_interaction;
    UiActionHandlers handlers;
    handlers.debug_open = [&]() { diagnostics.open(); };
    handlers.debug_close = [&]() { diagnostics.close(); };
    handlers.echo_mode = [&](int index) {
        diagnostics.set_echo_mode(index == 0   ? EchoCancellationMode::On
                                  : index == 1 ? EchoCancellationMode::RemoteOnly
                                               : EchoCancellationMode::Off);
    };
    handlers.mic_test = [&]() {
        std::string mic_error;
        diagnostics.run_mic_test(platform->audio.get(), &mic_error);
    };
    add_window_handlers(&handlers, platform->display.get());
    auto dismiss_overlay = [&]() -> bool {
        if (diagnostics.is_open()) {
            diagnostics.close();
            return true;
        }
        return false;
    };
    auto toggle_debug = [&]() {
        if (diagnostics.is_open()) {
            diagnostics.close();
        } else {
            diagnostics.open();
        }
    };
    auto handle_puck_pointer = [&](const InputEvent& event, float now) {
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        if (event.type == InputEvent::Type::PointerDown) {
            if (!ui.hit_map().hit(event.x, event.y) && !diagnostics.is_open()) {
                puck_interaction.pointer_down(event.x, event.y, now);
            }
        } else if (event.type == InputEvent::Type::PointerMove) {
            puck_interaction.pointer_move(event.x, event.y, now);
        } else if (event.type == InputEvent::Type::PointerUp) {
            puck_interaction.pointer_up(event.x, event.y, now);
        }
    };

    while (running && !platform->display->should_close()) {
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        const UiFocusResult hold = focus.update(elapsed);
        if (hold.action != UiAction::None) {
            apply_focus_result(hold, handlers, dismiss_overlay, toggle_debug, nullptr);
        }
        InputEvent event;
        while (platform->input && platform->input->poll(event)) {
            ui.notify_input(event);
            handle_puck_pointer(event, elapsed);
            const UiFocusResult focus_result = focus.handle(event, elapsed);
            if (apply_focus_result(focus_result, handlers, dismiss_overlay, toggle_debug,
                                   nullptr)) {
                continue;
            }

            int index = -1;
            const UiAction action = router.handle(event, &index);
            if (action == UiAction::Escape) {
                if (diagnostics.is_open()) {
                    diagnostics.close();
                } else {
                    running = false;
                }
                continue;
            }
            dispatch_ui_action(action, index, handlers);
        }
        if (router.quit_requested()) {
            running = false;
        }
        if (!running) {
            break;
        }

        ui.set_debug(diagnostics.view());
        puck_interaction.set_bounds(ui.puck_bounds(), ui.puck_stage());
        puck_interaction.update(elapsed);
        PuckVisual visual;
        visual.pose = puck_interaction.pose();
        ui.set_visual(visual);
        platform->display->make_current();
        media.poll();
        focus.sync();
        ui.set_focus(focus.view(elapsed, ui.hints_visible()));
        renderer.begin(platform->display->size(), platform->display->dpi_scale());
        ui.draw(renderer.canvas(), platform->display->size(), elapsed);
        renderer.end();
        focus.sync();
        platform->display->swap();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return 0;
}

}  // namespace pk
