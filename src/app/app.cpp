#include "app/app.h"

#include "app/assets.h"
#include "app/call_flow.h"
#include "app/car_ui.h"
#include "app/diagnostics.h"
#include "app/media_overlay.h"
#include "app/pairing.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
#include "app/session_startup.h"
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

#include <atomic>
#include <chrono>
#include <cstdlib>
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

// run_session() returns this after a shared-device cleanup so App::run can
// create a fresh session and show the QR without tearing down the window.
constexpr int kRestartPairing = 3;

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

    // One lock covers the whole batch so a parallel fetch cannot interleave
    // with session or actor traffic on the same client.
    std::vector<HttpBatchResponse> request_many(
        const std::vector<HttpRequest>& requests) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->request_many(requests);
    }

    std::string export_cookies() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->export_cookies();
    }

    void import_cookies(const std::string& cookies) override {
        std::lock_guard<std::mutex> lock(mutex_);
        inner_->import_cookies(cookies);
    }

    void clear_cookies() override {
        std::lock_guard<std::mutex> lock(mutex_);
        inner_->clear_cookies();
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
            communicated_ = false;
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

    // Web observer readiness is "this socket has received server traffic",
    // not a guessed snapshot event. The first frame on a new socket is the
    // resume acknowledgement (which may be a JSON-RPC void response).
    bool server_communicated() {
        std::lock_guard<std::mutex> lock(mutex_);
        return communicated_;
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
                communicated_ = true;
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
    bool communicated_ = false;
};

// One per call. The voice worker's data callback owns a shared_ptr to this
// mailbox, not a reference to the app's unique_ptr<CallController>, so a
// hangup can reset the controller while an in-flight callback is still
// returning. The main loop consumes the queues.
struct CallInbox {
    static constexpr std::size_t kMaxReplayEvents = 500;
    static constexpr std::size_t kMaxReplayBytes = 16000;

    void on_data(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        data.push_back(text);
        if (!buffer_replay) {
            return;
        }
        const nlohmann::json event = nlohmann::json::parse(text, nullptr, false);
        if (!event.is_object() || !event.contains("type") ||
            !event["type"].is_string()) {
            return;
        }
        const std::string type = event["type"].get<std::string>();
        // Web bufferEvent(): audio deltas stay on the media path and are never
        // replayed into the actor; oversized events are dropped.
        if (type.rfind("session.output_audio.", 0) == 0 ||
            type.rfind("session.input_audio.", 0) == 0 ||
            text.size() > kMaxReplayBytes) {
            return;
        }
        if (replay.size() >= kMaxReplayEvents) {
            replay.pop_front();
        }
        replay.push_back(event);
    }

    void begin_replay() {
        std::lock_guard<std::mutex> lock(mutex);
        buffer_replay = true;
        replay.clear();
    }

    std::deque<nlohmann::json> take_replay() {
        std::lock_guard<std::mutex> lock(mutex);
        buffer_replay = false;
        std::deque<nlohmann::json> taken;
        taken.swap(replay);
        return taken;
    }

    std::deque<std::string> take_data() {
        std::lock_guard<std::mutex> lock(mutex);
        std::deque<std::string> taken;
        taken.swap(data);
        return taken;
    }

    std::mutex mutex;
    std::deque<std::string> data;
    std::deque<nlohmann::json> replay;
    bool buffer_replay = false;
};

struct ActorPrepareResult {
    bool ok = false;
    CarSession session;
    std::unique_ptr<WebSocket> socket;
    std::string error;
};

// Token refresh and the WebSocket handshake can each take seconds. They run
// here, off the UI thread; the main loop swaps in the finished socket. Every
// start gets a copy of the session so the worker never reads fields the UI is
// mutating.
class AsyncActorPrepare {
public:
    using Worker =
        std::function<bool(CarSession*, std::unique_ptr<WebSocket>*, std::string*)>;

    ~AsyncActorPrepare() { stop(); }

    // A finished result is still "busy" until the UI takes it; otherwise a
    // second start would join the worker and overwrite the socket it produced.
    bool busy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || done_;
    }

    bool start(Worker worker, const CarSession& session) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ || done_) {
                return false;
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ || done_) {
                return false;
            }
            running_ = true;
            done_ = false;
            result_ = ActorPrepareResult{};
        }
        const CarSession snapshot = session;
        thread_ = std::thread([this, worker = std::move(worker), snapshot]() mutable {
            ActorPrepareResult result;
            result.session = snapshot;
            std::unique_ptr<WebSocket> socket;
            std::string error;
            result.ok = worker(&result.session, &socket, &error);
            result.socket = std::move(socket);
            result.error = std::move(error);
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = std::move(result);
            running_ = false;
            done_ = true;
        });
        return true;
    }

    bool take(ActorPrepareResult* out) {
        std::thread finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!done_) {
                return false;
            }
            *out = std::move(result_);
            result_ = ActorPrepareResult{};
            done_ = false;
            if (thread_.joinable() && !running_) {
                finished = std::move(thread_);
            }
        }
        if (finished.joinable()) {
            finished.join();
        }
        return true;
    }

    void stop() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (thread_.joinable()) {
                thread = std::move(thread_);
            }
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    mutable std::mutex mutex_;
    std::thread thread_;
    ActorPrepareResult result_;
    bool running_ = false;
    bool done_ = false;
};

// The rotating car-session cookie is renewed while the car screen is live.
// The HTTP call is slow-network bounded by curl's timeout, so it runs here
// rather than stalling tick/input/render.
class AsyncSessionRenew {
public:
    using Worker = std::function<bool(std::string*)>;

    ~AsyncSessionRenew() { stop(); }

    bool busy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || done_;
    }

    bool start(Worker worker) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ || done_) {
                return false;
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ || done_) {
                return false;
            }
            running_ = true;
            error_.clear();
        }
        thread_ = std::thread([this, worker = std::move(worker)]() {
            std::string error;
            const bool ok = worker(&error);
            std::lock_guard<std::mutex> lock(mutex_);
            ok_ = ok;
            error_ = std::move(error);
            running_ = false;
            done_ = true;
        });
        return true;
    }

    bool take(bool* ok, std::string* error) {
        std::thread finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!done_) {
                return false;
            }
            *ok = ok_;
            *error = error_;
            done_ = false;
            if (thread_.joinable() && !running_) {
                finished = std::move(thread_);
            }
        }
        if (finished.joinable()) {
            finished.join();
        }
        return true;
    }

    void stop() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (thread_.joinable()) {
                thread = std::move(thread_);
            }
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    bool done_ = false;
    bool ok_ = false;
    std::string error_;
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

int run_session(const AppOptions& options, Platform& platform, Renderer& renderer, CarUi& ui,
                MediaOverlay& media, PluginUi& plugins, PasskeyFlow& passkey,
                Diagnostics& diagnostics, const PairingOptions& pairing_options,
                std::unique_ptr<PuckView>& puck) {
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

    const auto started = Clock::now();
    const std::string backend_name = platform.display->backend_name();
    // A single-frame diagnostic run waits for the ready state so its PPM is the
    // QR or connecting screen. The default desktop window (frames=0) renders
    // the loading state immediately instead.
    const bool snapshot_mode = backend_name == "headless" || backend_name == "win32-headless" ||
                               (backend_name == "win32" && options.platform.frames > 0);
    // Asset downloads run on their own thread, so they get their own client:
    // sharing the serialized session client would queue every download behind
    // session validation and remote-hash discovery and delay the QR.
    std::string asset_http_error;
    auto asset_http = create_http_client(&asset_http_error);
    if (!asset_http) {
        log::warn("%s; asset downloads share the session client", asset_http_error.c_str());
    }
    AssetFetcher assets(asset_http ? *asset_http : *platform.http);
    bool puck_upgraded = false;
    auto upgrade_puck = [&]() -> bool {
        if (puck_upgraded || !assets.finished()) {
            return false;
        }
        platform.display->make_current();
        auto next = create_puck_view(options.enable_3d);
        ui.set_puck(*next);
        puck = std::move(next);
        puck_upgraded = true;
        log::info("Puck renderer: %s", puck->name());
        return true;
    };

    SessionStartup startup(*platform.http, platform.secure_store.get(), pairing_options,
                           options.pair || options.temporary_session);
    startup.start();
    assets.start();

    auto elapsed_seconds = [&]() {
        return std::chrono::duration<float>(Clock::now() - started).count();
    };
    UiFocus focus;
    focus.set_hit_map(&ui.hit_map());
    float script_time = 0.0f;
    bool use_script_time = false;
    PuckInteraction puck_interaction;
    auto ui_time = [&]() { return use_script_time ? script_time : elapsed_seconds(); };
    bool first_frame_logged = false;
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
            if (!first_frame_logged) {
                first_frame_logged = true;
                log::info("startup: first frame presented %.0f ms after launch",
                          std::chrono::duration<float, std::milli>(Clock::now() - started).count());
            }
        }
    };

    UiInputRouter router;
    router.set_hit_map(&ui.hit_map());

    bool dirty = true;
    bool quit = false;
    bool retry_requested = false;
    UiActionHandlers debug_handlers;
    debug_handlers.pairing_retry = [&]() { retry_requested = true; };
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

    // Startup runs off the UI thread. The window shows a loading state while
    // the saved session is validated and, when needed, the pairing challenge
    // is prepared; the QR appears as soon as the worker is done. Snapshot runs
    // wait for the ready state so their single PPM is the QR or connecting
    // screen instead of a loading frame.
    if (snapshot_mode) {
        startup.wait();
        assets.wait();
        upgrade_puck();
    } else {
        while (!quit) {
            poll_debug_input();
            if (upgrade_puck()) {
                dirty = true;
            }
            const SessionStartupStatus session_status = startup.status();
            const AssetFetchStatus asset_status = assets.status();
            std::string loading_status = session_status.phase;
            if (!asset_status.finished && asset_status.total > 0) {
                loading_status = "Fetching assets " +
                                 std::to_string(asset_status.completed) + "/" +
                                 std::to_string(asset_status.total);
            }
            // Keep the window at the desktop cadence while startup runs so the
            // Puck and the loading status are live, not a frozen first frame.
            ui.set_pairing(QrCode{}, "Call Puck", loading_status);
            sync_pairing();
            render(true);
            dirty = false;
            if (session_status.finished &&
                (!session_status.resumed || asset_status.finished)) {
                break;
            }
            if (platform.display->should_close()) {
                quit = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (quit) {
            startup.wait();
            assets.wait();
            return 0;
        }
        startup.wait();
    }

    const SessionStartupStatus startup_status = startup.status();
    if (startup_status.failed) {
        log::error("startup failed: %s", startup_status.error.c_str());
        assets.wait();
        return 1;
    }

    PairingSession& pairing = startup.pairing();
    CarSession session;
    const bool resumed = startup.resumed();
    if (resumed) {
        session = startup.session();
        log::info("startup: restored car session for thread %s; entering the car path",
                  session.thread_id.c_str());
    }
    std::string error;

    auto show_challenge = [&](const std::string& status, bool announce) {
        ui.set_pairing(pairing.qr(), "Call Puck",
                       status.empty() ? std::string("Waiting for scan") : status);
        ui.set_pairing_recovery(PairingRecovery::None);
        sync_pairing();
        render(true);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
        if (announce) {
            std::cout << pairing.pairing_url() << std::endl;
            log::info("pairing: QR ready");
        }
    };

    auto restart_pairing = [&]() -> bool {
        std::string restart_error;
        if (!pairing.restart(&restart_error)) {
            log::warn("pairing retry: %s", restart_error.c_str());
            return false;
        }
        return true;
    };

    if (!resumed) {
        show_challenge("Waiting for scan", true);
    } else {
        ui.set_pairing(pairing.qr(), "Connecting", "Restoring session");
        sync_pairing();
        render(true);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
    }
    if (platform.display->should_close() && options.input_script.empty()) {
        assets.wait();
        return 0;
    }
    if (!options.input_script.empty()) {
        assets.wait();
        upgrade_puck();
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

    auto wait_for_retry = [&](const std::string& title, const std::string& message,
                              int failed_generation) -> bool {
        // The failed round's QR and confirmation code are no longer valid.
        // Keep the status text and offer a fresh round instead.
        ui.set_pairing(QrCode{}, title, message);
        ui.set_pairing_recovery(PairingRecovery::Retry);
        sync_pairing();
        render(true);
        if (options.dump_targets) {
            dump_targets(ui.hit_map());
        }
        auto last_render = Clock::now();
        while (!quit && !platform.display->should_close()) {
            poll_debug_input();
            if (upgrade_puck()) {
                dirty = true;
            }
            if (pairing.challenge_generation() != failed_generation) {
                // Leave the restart's update for the main pairing loop; it
                // may already contain the new scanned code.
                show_challenge("Waiting for scan", true);
                return true;
            }
            if (retry_requested) {
                retry_requested = false;
                // Re-check the round at click time: the UI may have rendered
                // an older state than the worker's latest terminal reply.
                ui.set_pairing_recovery(PairingRecovery::Retrying);
                sync_pairing();
                render(true);
                if (!restart_pairing()) {
                    ui.set_pairing_recovery(PairingRecovery::Retry);
                    sync_pairing();
                    render(true);
                }
                dirty = true;
            }
            if (puck_interaction.active()) {
                dirty = true;
            }
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
        return false;
    };

    auto run_pairing_and_setup = [&]() -> bool {
        for (;;) {
            int generation = pairing.challenge_generation();
            std::string shown_code;
            std::string shown_message = "Waiting for scan";
            bool showing_code = false;
            PairingState last_state = PairingState::Idle;
            const auto deadline = Clock::now() + std::chrono::seconds(options.pair_timeout_seconds);
            PairingStatus status;
            bool paired = false;
            auto last_render = Clock::now();
            Clock::time_point scanned_at{};
            bool auto_retry_done = false;

            while (!quit && !platform.display->should_close()) {
                poll_debug_input();
                if (upgrade_puck()) {
                    dirty = true;
                }

                PairingUpdate update;
                while (pairing.take_update(&update)) {
                    if (update.stale) {
                        continue;
                    }
                    status = update.status;
                    if (status.state == PairingState::Paired && options.pair_retry_on_paired) {
                        // Simulate the user pressing Try Again just before this
                        // Paired snapshot is consumed. The retry branch below
                        // must win and discard the account.
                        debug_handlers.pairing_retry();
                    }
                    if (status.state != last_state) {
                        last_state = status.state;
                        log::info("pairing state: %s", status.message.c_str());
                    }
                    const bool challenge_changed = update.generation != generation;
                    if (challenge_changed) {
                        generation = update.generation;
                        shown_code.clear();
                        showing_code = false;
                        log::info("pairing challenge changed; scan the new QR");
                    }
                    if (status.state == PairingState::Scanned && !status.code.empty()) {
                        if (status.code != shown_code || status.message != shown_message) {
                            const bool code_changed = status.code != shown_code;
                            shown_code = status.code;
                            showing_code = true;
                            shown_message = status.message;
                            scanned_at = Clock::now();
                            ui.set_pairing(pairing.qr(), "Confirm on Your Device",
                                           status.message.empty()
                                               ? std::string("Confirm on your device")
                                               : status.message,
                                           status.code);
                            // The confirmation page owns the same Try Again
                            // action as a failed round. Pressing it abandons
                            // this challenge; any late paired reply is stale.
                            ui.set_pairing_recovery(PairingRecovery::Retry);
                            sync_pairing();
                            render(true);
                            dirty = false;
                            if (code_changed) {
                                log::info("pairing scanned; code %s", status.code.c_str());
                            }
                        }
                    } else if (status.state == PairingState::Waiting ||
                               status.state == PairingState::Failed) {
                        if (showing_code || challenge_changed ||
                            status.message != shown_message) {
                            showing_code = false;
                            shown_code.clear();
                            shown_message = status.message;
                            show_challenge(status.message, challenge_changed);
                            dirty = false;
                        }
                    }
                    if (status.state == PairingState::Paired) {
                        // The confirmation page is no longer actionable; the
                        // app takes ownership of this generation immediately.
                        showing_code = false;
                        shown_code.clear();
                        shown_message.clear();
                        ui.set_pairing_recovery(PairingRecovery::None);
                        paired = true;
                        break;
                    }
                }

                // Try Again is live on both the failure page and the
                // confirmation page. Checking it here, rather than only in
                // wait_for_retry(), keeps the scan loop's own poll and input
                // paths honest.
                if (options.pair_retry_after_ms >= 0 && showing_code &&
                    !auto_retry_done &&
                    Clock::now() - scanned_at >=
                        std::chrono::milliseconds(options.pair_retry_after_ms)) {
                    // Exercise the same callback the confirmation page's
                    // Try Again target dispatches to.
                    debug_handlers.pairing_retry();
                    auto_retry_done = true;
                }
                if (retry_requested) {
                    retry_requested = false;
                    // A Retry already requested by the user (or the test hook)
                    // wins over an unconsumed Paired update in this same pass.
                    // restart() discards that paired session; do not let the
                    // stale local decision establish it on the next loop.
                    paired = false;
                    showing_code = false;
                    shown_code.clear();
                    shown_message.clear();
                    ui.set_pairing_recovery(PairingRecovery::Retrying);
                    ui.set_pairing(pairing.qr(), "Pairing",
                                   "Generating a new pairing code", "");
                    sync_pairing();
                    render(true);
                    dirty = false;
                    if (restart_pairing()) {
                        generation = pairing.challenge_generation();
                        show_challenge("Waiting for scan", true);
                    } else {
                        ui.set_pairing_recovery(PairingRecovery::Retry);
                        ui.set_pairing(QrCode{}, "Pairing Unavailable",
                                       "Could not generate a new pairing code",
                                       "");
                        sync_pairing();
                        render(true);
                        dirty = false;
                    }
                    continue;
                }
                if (paired) {
                    break;
                }
                if (status.state == PairingState::Failed ||
                    status.state == PairingState::Expired) {
                    break;
                }
                if (Clock::now() >= deadline) {
                    pairing.expire("Pairing timed out");
                    status.state = PairingState::Expired;
                    status.message = "Pairing timed out";
                    break;
                }

                if (puck_interaction.active()) {
                    dirty = true;
                }
                const bool headless = std::string(platform.display->backend_name()) == "headless";
                const bool desktop = std::string(platform.display->backend_name()) == "win32";
                if (dirty || (diagnostics.is_open() && !headless &&
                              Clock::now() - last_render >= std::chrono::milliseconds(100)) ||
                    (desktop &&
                     Clock::now() - last_render >= std::chrono::milliseconds(16))) {
                    sync_pairing();
                    render(true);
                    last_render = Clock::now();
                    dirty = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }

            if (quit || platform.display->should_close()) {
                return false;
            }
            if (!paired) {
                // Do not join an in-flight claim here: expire() has already
                // published the terminal state, and the old request can
                // finish and clean its stale session in the background.
                const bool expired = status.state == PairingState::Expired;
                const std::string title = expired ? "Pairing Timed Out" : "Pairing Failed";
                const std::string message =
                    status.message.empty() ? std::string("Try Again") : status.message;
                const int failed_generation = pairing.challenge_generation();
                if (!wait_for_retry(title, message, failed_generation)) {
                    return false;
                }
                continue;
            }

            pairing.stop_polling();
            ui.set_pairing(pairing.qr(), "Connecting", "Paired. Starting session...");
            sync_pairing();
            render(true);

            std::string setup_error;
            bool setup_ok = pairing.establish_session(&error);
            if (!setup_ok) {
                setup_error = "Cannot create the car session: " + error;
            } else if (!pairing.load_car_session(&session, &error)) {
                setup_ok = false;
                setup_error = "Cannot load the car session: " + error;
            }
            if (setup_ok) {
                if (options.temporary_session) {
                    // The session and cookie stay in memory for this kiosk
                    // visit only. A crash or power loss must not leave the
                    // previous user's account for the next driver.
                    log::info("temporary car session for thread %s (not persisted)",
                              session.thread_id.c_str());
                } else if (pairing.persist(session, &error)) {
                    log::info("car session saved for thread %s", session.thread_id.c_str());
                } else {
                    log::warn("cannot persist car session: %s", error.c_str());
                }
                pairing.acknowledge_paired();
                return true;
            }

            log::error("car session setup: %s", setup_error.c_str());
            const int failed_generation = pairing.challenge_generation();
            if (!wait_for_retry("Cannot Start Puck", setup_error, failed_generation)) {
                return false;
            }
        }
    };

    if (!resumed) {
        if (!run_pairing_and_setup()) {
            pairing.stop_polling();
            assets.wait();
            return 0;
        }
    }

    log::info("car session ready: thread=%s actor=%s capability=%s", session.thread_id.c_str(),
              redact_url(session.actor_base_url).c_str(),
              session.capability.empty() ? "default" : session.capability.c_str());

    AuthorizationWatcher authorization(pairing);

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

    auto leave_temporary_session = [&]() {
        // Stop polling before the DELETE so a late claim cannot recreate the
        // account after the local jar has been cleared.
        pairing.stop_polling();
        if (!options.temporary_session) {
            return;
        }
        std::string leave_error;
        pairing.leave_session(&leave_error);
        if (!leave_error.empty()) {
            log::warn("temporary session cleanup: %s", redact_text(leave_error).c_str());
        }
    };

    if (!platform.websockets) {
        log::error("WebSocket factory is unavailable");
        leave_temporary_session();
        if (options.temporary_session && !quit && !platform.display->should_close()) {
            return kRestartPairing;
        }
        return 2;
    }

    const std::string page_load_id = random_uuid();
    const std::vector<HttpHeader> headers = {
        {"Origin", pairing.origin()},
        {"User-Agent", "puck-kiosk/0.1"},
    };
    std::atomic<int> resume_id{0};
    auto open_actor_socket = [&](CarSession* next, std::string* prepare_error) {
        ActorEndpoint endpoint;
        if (!build_actor_endpoint(*next, page_load_id, &endpoint, prepare_error)) {
            return std::unique_ptr<WebSocket>{};
        }
        log::info("actor: connecting to %s", endpoint.display_url.c_str());
        auto socket = platform.websockets->connect(endpoint.url, headers,
                                                   endpoint.subprotocols, prepare_error);
        if (!socket) {
            return socket;
        }
        const std::string resume = nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", "puck-kiosk-" + std::to_string(resume_id.fetch_add(1))},
            {"method", "client_resume"},
            {"params", {{"version", 0}}},
        }.dump();
        if (!socket->send_text(resume)) {
            if (prepare_error) {
                *prepare_error = "cannot send client_resume";
            }
            return std::unique_ptr<WebSocket>{};
        }
        return socket;
    };

    // The first token, readiness probe, and socket handshake all touch the
    // network. Keep the pairing UI responsive and render a connecting state
    // instead of blocking the window on the initial attach.
    AsyncActorPrepare initial_prepare;
    std::atomic<bool> initial_actor_token_ok{false};
    const bool initial_started = initial_prepare.start(
        [&](CarSession* next, std::unique_ptr<WebSocket>* out, std::string* prepare_error) {
            std::string token_error;
            if (pairing.fetch_actor_token(next, &token_error)) {
                initial_actor_token_ok = true;
            } else {
                log::warn("thread actor token: %s (trying the gateway token)",
                          redact_text(token_error).c_str());
            }
            std::string ready_error;
            if (wait_actor_ready(*platform.http, *next, page_load_id, &ready_error)) {
                log::info("actor: ready");
            } else {
                log::warn("actor ready check: %s", redact_text(ready_error).c_str());
            }
            *out = open_actor_socket(next, prepare_error);
            return *out != nullptr;
        },
        session);
    if (!initial_started) {
        log::error("actor: cannot start the initial connection worker");
        leave_temporary_session();
        if (options.temporary_session && !quit && !platform.display->should_close()) {
            return kRestartPairing;
        }
        return 2;
    }

    bool initial_rendered = false;
    auto last_initial_render = Clock::now();
    ActorPrepareResult initial_prepared;
    while (!initial_prepare.take(&initial_prepared)) {
        // This loop runs while the first token/readiness/WebSocket handshake is
        // in flight. Pump the platform input here too: Win32 delivers messages
        // only through input.poll(), so rendering alone would leave a slow
        // handshake window unable to close or accept Escape.
        poll_debug_input();
        if (quit || platform.display->should_close()) {
            initial_prepare.stop();
            leave_temporary_session();
            assets.wait();
            return 0;
        }
        if (upgrade_puck()) {
            dirty = true;
        }
        if (!initial_rendered || dirty ||
            Clock::now() - last_initial_render >= std::chrono::milliseconds(100)) {
            ui.set_pairing(pairing.qr(), "Connecting", "Preparing the actor connection");
            ui.set_pairing_recovery(PairingRecovery::None);
            sync_pairing();
            render(true);
            initial_rendered = true;
            dirty = false;
            last_initial_render = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    initial_prepare.stop();

    std::unique_ptr<WebSocket> websocket;
    if (!initial_prepared.ok || !initial_prepared.socket) {
        log::error("actor connect: %s", redact_text(initial_prepared.error).c_str());
        leave_temporary_session();
        if (options.temporary_session && !quit && !platform.display->should_close()) {
            return kRestartPairing;
        }
        return 2;
    }
    websocket = std::move(initial_prepared.socket);
    session.ws_token = initial_prepared.session.ws_token;
    double token_exp = ws_token_exp_seconds(session.ws_token);
    // Capability used to mint the token on the socket that is currently
    // installed. A capability change during an async handshake must not let a
    // read-only socket masquerade as the write socket.
    std::string socket_capability =
        initial_actor_token_ok ? initial_prepared.session.capability : "read";

    ActorReader reader;
    reader.start(websocket.get());
    // True only after the current socket has received server traffic. A TCP or
    // WebSocket open is not enough: RivetKit opens the socket before the actor
    // and only answers client_resume once the observer is attached.
    bool observer_ready = false;
    plugins.set_sender([&](const std::string& text) {
        return websocket && websocket->is_open() && websocket->send_text(text);
    });

    log::info("actor: connected; observing for %d seconds", options.actor_seconds);

    if (!explicit_write_permission(session)) {
        authorization.start(session);
    }

    std::unique_ptr<voice::VoiceEngine> voice_engine;
    std::unique_ptr<CallController> call;
    std::shared_ptr<CallInbox> call_inbox;
    voice::VoiceEngineStats final_stats;
    CallView ended_view;
    std::string call_error_text;
    bool call_failed = false;
    bool ended_by_user = false;
    bool signaling_down = false;
    bool attach_pending = false;
    std::string attach_request_id;
    std::string attach_error;
    Clock::time_point attach_deadline{};
    int attach_counter = 0;
    int replay_counter = 0;
    // The previous call's clean close was not confirmed. A new call must not
    // reuse that actor connection; reconnect first and retry the old stop.
    bool redial_needs_fresh_socket = false;
    bool cleanup_stop_pending = false;
    std::string cleanup_stop_request_id;
    Clock::time_point cleanup_deadline{};
    int cleanup_generation = 0;
    int cleanup_counter = 0;
    // Incremented whenever the installed socket changes. An unconfirmed close
    // may only be cleared by a stop on a newer generation whose observer has
    // already received its resume acknowledgement.
    int socket_generation = 1;
    bool fresh_socket_required = false;
    auto next_reconnect = Clock::now();
    bool prepare_wanted = false;
    bool prepare_refresh = false;
    auto start_call = [&]() -> bool {
        if (!platform.audio) {
            log::error("voice: no audio backend is available");
            return false;
        }
        if (call) {
            // The old call must finish its close handshake and be released
            // before another one can start on this actor connection.
            log::warn("voice: refusing to start a second call before the first is released");
            return false;
        }
        voice_engine.reset();
        call_inbox.reset();
        ended_view = CallView{};
        call_error_text.clear();
        call_failed = false;
        signaling_down = false;
        attach_pending = false;
        attach_request_id.clear();
        attach_error.clear();
        voice::VoiceEngineOptions voice_options;
        voice_options.enable_aec = diagnostics.echo_mode() == EchoCancellationMode::On;
        voice_engine = std::make_unique<voice::VoiceEngine>(*platform.audio, voice_options);
        CallOptions call_options;
        call_options.mute_after_seconds = options.mute_after_seconds;
        call_options.end_after_seconds = options.end_after_seconds;
        call = std::make_unique<CallController>(*websocket, *voice_engine, call_options);
        // The call back-pointer is reset by the UI thread on hangup. The data
        // callback must outlive that reset, so it captures a per-call mailbox
        // and only the main loop talks to the controller.
        call_inbox = std::make_shared<CallInbox>();
        const std::shared_ptr<CallInbox> inbox = call_inbox;
        voice_engine->set_data_callback(
            [inbox](const std::string& text) { inbox->on_data(text); });
        std::string call_error;
        if (!call->start(&call_error)) {
            log::error("voice: cannot start call: %s", call_error.c_str());
            call_error_text = call_error;
            call_failed = true;
            ended_view = CallView{};
            ended_view.phase = CallPhase::Failed;
            ended_view.caption = "Call failed";
            ended_view.detail = call_error + " — tap End, then Call to try again";
            call.reset();
            voice_engine.reset();
            call_inbox.reset();
            return false;
        }
        return true;
    };

    int message_count = 0;
    int passkey_request_id = 0;
    int next_request_id = 1000;
    auto last_ping = Clock::now();
    auto last_car_render = Clock::now();
    auto last_renew = Clock::now();
    AsyncSessionRenew renew_task;
    const auto actor_deadline = Clock::now() + std::chrono::seconds(options.actor_seconds);
    const bool observe_forever = options.actor_seconds == 0;

    auto update_passkey_context = [&]() {
        PasskeyContext context;
        context.car_session = true;
        context.actor_connected = websocket && websocket->is_open();
        context.can_write = explicit_write_permission(session);
        context.requires_sudo = session.requires_sudo_for_write;
        passkey.set_context(context);
    };

    auto send_attach = [&]() -> bool {
        if (!call || !websocket || !websocket->is_open()) {
            return false;
        }
        const std::string session_id = call->session_id();
        if (session_id.empty()) {
            return false;
        }
        const std::string request_id =
            "puck-voice-attach-" + std::to_string(++attach_counter);
        nlohmann::json params;
        params["transport"] = {{"type", "webrtc_attach"}, {"sessionID", session_id}};
        params["mediaKinds"] = nlohmann::json::array({"image"});
        const nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", request_id},
            {"method", "client_puck_voice_start"},
            {"params", params},
        };
        if (!websocket->send_text(request.dump())) {
            log::warn("voice: cannot send the reattach request");
            return false;
        }
        attach_request_id = request_id;
        attach_pending = true;
        attach_deadline = Clock::now() + std::chrono::seconds(5);
        log::info("voice: reattach sent for session %s",
                  redact_text(session_id).c_str());
        return true;
    };

    auto send_replay = [&](std::deque<nlohmann::json> events) -> bool {
        if (events.empty() || !websocket || !websocket->is_open()) {
            return events.empty();
        }
        nlohmann::json params;
        params["events"] = nlohmann::json::array();
        for (const nlohmann::json& event : events) {
            params["events"].push_back(event);
        }
        const nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", "puck-voice-replay-" + std::to_string(++replay_counter)},
            {"method", "client_puck_voice_replay"},
            {"params", params},
        };
        const bool sent = websocket->send_text(request.dump());
        if (!sent) {
            log::warn("voice: cannot replay %zu missed event(s)", events.size());
        } else {
            log::info("voice: replayed %zu event(s) missed while the actor was away",
                      events.size());
        }
        return sent;
    };

    auto send_cleanup_stop = [&]() -> bool {
        if (!websocket || !websocket->is_open() ||
            socket_generation <= cleanup_generation) {
            return false;
        }
        cleanup_stop_request_id =
            "puck-voice-cleanup-" + std::to_string(++cleanup_counter);
        const nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", cleanup_stop_request_id},
            {"method", "client_puck_voice_stop"},
            {"params", nlohmann::json::object()},
        };
        if (!websocket->send_text(request.dump())) {
            cleanup_stop_request_id.clear();
            return false;
        }
        cleanup_stop_pending = true;
        cleanup_deadline = Clock::now() + std::chrono::seconds(5);
        log::info("voice: retrying the previous stop on generation %d", socket_generation);
        return true;
    };

    AsyncActorPrepare actor_prepare;
    auto prepare_actor = [&](bool refresh_token) -> bool {
        const bool started = actor_prepare.start(
            [&, refresh_token](CarSession* next, std::unique_ptr<WebSocket>* out,
                               std::string* prepare_error) {
                if (refresh_token && !pairing.fetch_actor_token(next, prepare_error)) {
                    return false;
                }
                ActorEndpoint endpoint;
                if (!build_actor_endpoint(*next, page_load_id, &endpoint, prepare_error)) {
                    return false;
                }
                log::info("actor: connecting to %s", endpoint.display_url.c_str());
                auto socket = platform.websockets->connect(endpoint.url, headers,
                                                            endpoint.subprotocols,
                                                            prepare_error);
                if (!socket) {
                    return false;
                }
                const std::string resume = nlohmann::json{
                    {"jsonrpc", "2.0"},
                    {"id", "puck-kiosk-" + std::to_string(resume_id.fetch_add(1))},
                    {"method", "client_resume"},
                    {"params", {{"version", 0}}},
                }.dump();
                if (!socket->send_text(resume)) {
                    if (prepare_error) {
                        *prepare_error = "cannot send client_resume";
                    }
                    return false;
                }
                *out = std::move(socket);
                return true;
            },
            session);
        if (started) {
            log::info("actor: preparing a %s socket in the background",
                      refresh_token ? "refreshed" : "replacement");
        }
        return started;
    };

    auto request_prepare = [&](bool refresh_token) {
        prepare_refresh = prepare_refresh || refresh_token;
        if (actor_prepare.busy()) {
            prepare_wanted = true;
            return;
        }
        if (prepare_actor(prepare_refresh)) {
            prepare_wanted = false;
        } else {
            prepare_wanted = true;
        }
    };

    // A close that was not confirmed, or a cleanup stop that failed, may only
    // be retried on a new socket generation. Keep the old socket open for
    // media/reporting, but make it unable to unlock the call button.
    auto begin_fresh_socket_cleanup = [&]() {
        cleanup_stop_pending = false;
        cleanup_stop_request_id.clear();
        cleanup_deadline = Clock::time_point{};
        cleanup_generation = socket_generation;
        fresh_socket_required = true;
        observer_ready = false;
        signaling_down = true;
        next_reconnect = Clock::now();
        request_prepare(false);
    };

    auto apply_prepared = [&](ActorPrepareResult& prepared) -> bool {
        if (!prepared.ok || !prepared.socket) {
            return false;
        }
        if (prepared.session.capability != session.capability) {
            log::warn("actor: discarding a prepared %s socket after the capability became %s",
                      prepared.session.capability.empty() ? "default"
                                                         : prepared.session.capability.c_str(),
                      session.capability.empty() ? "default" : session.capability.c_str());
            return false;
        }
        const int previous_generation = socket_generation;
        reader.stop();
        if (websocket) {
            websocket->close();
        }
        websocket = std::move(prepared.socket);
        // Keep the UI's current capability: the worker only owns the token and
        // socket. A capability change during the handshake triggers another
        // preparation instead of silently connecting read-only after write.
        session.ws_token = prepared.session.ws_token;
        socket_capability = prepared.session.capability;
        token_exp = ws_token_exp_seconds(session.ws_token);
        // An attach sent on the old socket can never be acknowledged here.
        attach_pending = false;
        attach_request_id.clear();
        if (redial_needs_fresh_socket && cleanup_stop_pending) {
            // The receipt belonged to the old generation; replay the cleanup
            // again after this socket's observer is ready.
            cleanup_stop_pending = false;
            cleanup_stop_request_id.clear();
            cleanup_deadline = Clock::time_point{};
            cleanup_generation = previous_generation;
        }
        ++socket_generation;
        fresh_socket_required = false;
        reader.start(websocket.get());
        observer_ready = false;
        if (call) {
            call->rebind_actor(*websocket);
            if (call->active() && !call->ended() && !call->failed() &&
                !call->view().closing && !signaling_down) {
                // A planned token/write-access socket swap is still a
                // signaling gap for this call: media stays, events buffer,
                // and the observer-ready path reattaches.
                signaling_down = true;
                if (call_inbox) {
                    call_inbox->begin_replay();
                }
            }
        }
        // The new observer's snapshot is authoritative. Drop the old sheet
        // list now; answered IDs remain suppressed when the snapshot arrives.
        plugins.reset_requests(false);
        last_ping = Clock::now();
        log::info("actor: replacement socket installed (open=%d); waiting for observer readiness",
                  websocket && websocket->is_open() ? 1 : 0);
        return true;
    };

    // The main loop and the bounded exit drain must make the same decision
    // about a terminal call. In particular, an unconfirmed close on display
    // exit still needs the fresh-socket cleanup; a confirmed close must never
    // send an extra stop.
    auto finish_call_terminal = [&](bool user_hangup) {
        if (!call) {
            return false;
        }
        if (voice_engine) {
            final_stats = voice_engine->stats();
        }
        ended_view = call->view();
        const bool confirmed = ended_view.close_confirmed;
        call_failed = call->failed();
        call_error_text = call->last_error();
        media.close();
        plugins.reset_requests(false);
        call.reset();
        voice_engine.reset();
        call_inbox.reset();
        signaling_down = false;
        attach_pending = false;
        attach_request_id.clear();
        // The stop receipt / session.closed handshake must be complete before
        // another call may reuse this actor connection.
        redial_needs_fresh_socket = !confirmed;
        cleanup_stop_pending = false;
        cleanup_stop_request_id.clear();
        cleanup_deadline = Clock::time_point{};
        if (confirmed) {
            fresh_socket_required = false;
            if (user_hangup) {
                // A normal user hangup goes straight back to the green call
                // control, not through a transient Ended page.
                ended_view = CallView{};
                call_failed = false;
            }
        } else {
            log::warn("voice: close was not confirmed; forcing a fresh actor socket "
                      "before another call");
            begin_fresh_socket_cleanup();
        }
        ended_by_user = false;
        return confirmed;
    };

    // The coordinator remains the UI state for explicit write access. Its
    // token/socket callbacks are unused; the async worker above owns that I/O.
    WriteAccessCoordinator write_access([](std::string*) { return false; },
                                        [](std::string*) { return false; });
    if (explicit_write_permission(session)) {
        write_access.set_write(true);
        if (initial_actor_token_ok) {
            write_access.mark_ready();
        } else {
            request_prepare(true);
        }
    }

    auto write_wait_view = [&]() {
        PasskeyView view;
        // The existing call control stays disabled for Phone mode while the
        // write-authorized socket is being prepared.
        view.mode = PasskeyMode::Phone;
        view.title = "Securing Write Access";
        view.steps = {
            {"Refreshing write access",
             "The car is preparing a new authorized actor connection."},
            {"Waiting for the actor",
             "It retries automatically; the call button unlocks when ready."},
        };
        view.status = write_access.message();
        return view;
    };

    CallGate call_gate;
    call_gate.update(explicit_write_permission(session) && write_access.write_ready() &&
                         observer_ready,
                     websocket && websocket->is_open() && !signaling_down);
    // --call is the same explicit request as tapping the button. If the car is
    // read-only, the write socket is still being prepared, or the socket has
    // not received its resume acknowledgement, the request waits.
    if (options.call && call_gate.request()) {
        if (!start_call()) {
            const bool visible = backend_name != "headless" && backend_name != "win32-headless";
            if (!visible) {
                return 2;
            }
            log::warn("voice: waiting on the car screen; the call can be retried");
        }
    }

    auto apply_authorization = [&](const AuthorizationStatus& update) -> bool {
        if (!update.valid) {
            return true;
        }
        if (!update.error.empty()) {
            log::warn("car authorization: %s", redact_text(update.error).c_str());
            return true;
        }
        if (!update.changed) {
            return true;
        }
        session.capability = update.session.capability;
        session.requires_sudo_for_write = update.session.requires_sudo_for_write;
        log::info("car authorization: capability=%s",
                  session.capability.empty() ? "default" : session.capability.c_str());
        std::string persist_error;
        if (!options.temporary_session && !pairing.persist(session, &persist_error)) {
            log::warn("cannot persist the refreshed authorization: %s",
                      redact_text(persist_error).c_str());
        }
        if (explicit_write_permission(session)) {
            write_access.set_write(true);
            // Refresh the token and open a write socket off the UI thread.
            request_prepare(true);
            authorization.stop();
        } else {
            write_access.set_write(false);
            // A click made while write access was revoked must not fire if
            // access is granted again later; the user has to ask again.
            call_gate.reset();
        }
        update_passkey_context();
        return true;
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
        ui.set_passkey(write_access.waiting() && explicit_write_permission(session)
                           ? write_wait_view()
                           : passkey.view());
        ui.set_call_available(explicit_write_permission(session) && write_access.write_ready() &&
                              observer_ready && websocket && websocket->is_open() &&
                              !signaling_down && !redial_needs_fresh_socket &&
                              !(call && call->view().closing));
        if (voice_engine) {
            diagnostics.set_call_aec_status(call && call->active() && !call->ended() &&
                                                !call->failed(),
                                            voice_engine->aec_status());
        } else {
            diagnostics.set_call_aec_status(false, voice::AecStatus{});
        }
        ui.set_debug(diagnostics.view());
        diagnostics.set_call_active(call && call->active());
    };

    bool car_dirty = true;
    UiActionHandlers handlers;
    handlers.call = [&]() {
        // Re-check at click/activation time rather than trusting the frame
        // that drew the button: pairing, authorization, and the socket can
        // all change between press and release.
        call_gate.update(explicit_write_permission(session) && write_access.write_ready() &&
                             observer_ready && !redial_needs_fresh_socket &&
                             !(call && call->view().closing),
                         websocket && websocket->is_open() && !signaling_down);
        if (call_gate.request()) {
            start_call();
        }
    };
    handlers.mute = [&]() {
        if (call && !call->view().closing) {
            call->mute(!call->view().muted);
        }
    };
    handlers.end = [&]() {
        if (call) {
            if (call->view().closing) {
                // The close handshake is already in flight; a second End or a
                // mute press must not send another stop or reopen audio.
                return;
            }
            ended_by_user = true;
            call->end("ended from the car screen");
            return;
        }
        if (ended_view.phase == CallPhase::Ended || ended_view.phase == CallPhase::Failed) {
            // The Web keeps the failed call object until the user ends it;
            // the next frame is the normal green call control. A close that
            // was not confirmed still forces the fresh-socket cleanup first.
            ended_view = CallView{};
            call_failed = false;
            if (redial_needs_fresh_socket) {
                begin_fresh_socket_cleanup();
            }
            car_dirty = true;
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
        const nlohmann::json parsed = nlohmann::json::parse(message, nullptr, false);
        if (attach_pending && !attach_request_id.empty()) {
            auto check_attach = [&](const nlohmann::json& value) {
                if (!value.is_object() || !value.contains("id") ||
                    !value["id"].is_string() ||
                    value["id"].get<std::string>() != attach_request_id) {
                    return false;
                }
                if (!value.contains("jsonrpc") || !value["jsonrpc"].is_string() ||
                    value["jsonrpc"].get<std::string>() != "2.0" ||
                    value.contains("method")) {
                    return false;
                }
                attach_pending = false;
                attach_request_id.clear();
                if (value.contains("error")) {
                    attach_error = redact_json(value["error"].dump(), 200);
                    log::warn("voice: reattach rejected: %s", attach_error.c_str());
                    signaling_down = false;
                    if (call_inbox) {
                        call_inbox->take_replay();
                    }
                    if (call) {
                        call->fail_recovery("voice reattach rejected: " + attach_error);
                    }
                } else {
                    // A void result is a valid JSON-RPC success.
                    signaling_down = false;
                    log::info("voice: reattach acknowledged");
                    if (call_inbox) {
                        send_replay(call_inbox->take_replay());
                    }
                }
                return true;
            };
            if (parsed.is_array()) {
                for (const nlohmann::json& entry : parsed) {
                    if (check_attach(entry)) {
                        break;
                    }
                }
            } else {
                check_attach(parsed);
            }
        }
        if (cleanup_stop_pending && !cleanup_stop_request_id.empty()) {
            auto check_cleanup = [&](const nlohmann::json& value) {
                if (!value.is_object() || !value.contains("id") ||
                    !value["id"].is_string() ||
                    value["id"].get<std::string>() != cleanup_stop_request_id) {
                    return false;
                }
                if (!value.contains("jsonrpc") || !value["jsonrpc"].is_string() ||
                    value["jsonrpc"].get<std::string>() != "2.0" ||
                    value.contains("method")) {
                    return false;
                }
                // The actor's void success is an id-only JSON-RPC response.
                // What matters is that this is a response-shaped frame with no
                // error; a method call with the same id is not a receipt.
                if (value.contains("error")) {
                    log::warn("voice: previous-session stop retry rejected: %s",
                              redact_json(value["error"].dump(), 200).c_str());
                    // A rejected stop is not a cleanup. Keep the gate closed
                    // and retry on another socket generation.
                    begin_fresh_socket_cleanup();
                    return true;
                }
                if (socket_generation <= cleanup_generation) {
                    return true;
                }
                log::info("voice: previous-session stop retry acknowledged on generation %d",
                          socket_generation);
                cleanup_stop_pending = false;
                cleanup_stop_request_id.clear();
                cleanup_deadline = Clock::time_point{};
                redial_needs_fresh_socket = false;
                return true;
            };
            if (parsed.is_array()) {
                for (const nlohmann::json& entry : parsed) {
                    if (check_cleanup(entry)) {
                        break;
                    }
                }
            } else {
                check_cleanup(parsed);
            }
        }
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

    bool exit_after_script_leave = false;
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
            // Leave through the common close path so an active call drains,
            // and shared mode can DELETE and clear the cookie jar.
            log::info("input script finished; leaving the car screen");
            exit_after_script_leave = true;
        } else {
            use_script_time = false;
        }
    }

    if (options.input_script.empty()) {
        // Fill in the car layout before the first input poll so the Puck
        // interaction does not use the pairing screen's rect.
        update_passkey_context();
        sync_car();
        render(false);
        sync_car();
    }

    bool running = !quit && !exit_after_script_leave;
    bool restart_after_leave = false;
    bool draining_cleanup = false;
    auto cleanup_exit_deadline = Clock::now() + std::chrono::seconds(10);
    int reconnect_attempts = 0;
    // Any visible backend keeps a failed/ended call on screen with a route
    // back to the call button. Only the headless CLI exits after its report.
    const bool desktop_ui = backend_name != "headless" && backend_name != "win32-headless";
    auto temporary_idle_deadline = Clock::now() + std::chrono::minutes(3);
    auto on_observer_ready = [&]() {
        if (observer_ready || fresh_socket_required || !reader.server_communicated()) {
            return;
        }
        observer_ready = true;
        log::info("actor: observer ready on the current socket");
        if (explicit_write_permission(session) && socket_capability == "write") {
            // The current socket has now received its resume ack. Whether this
            // is the initial write socket or a replacement prepared after a
            // read-to-write change, the gate can open.
            write_access.mark_ready();
        }
        if (call && call->active() && !call->ended() && !call->failed() &&
            !call->view().closing) {
            if (call->session_id().empty()) {
                call->fail_recovery("signaling was lost before the voice session was established");
            } else if (!attach_pending) {
                send_attach();
            }
        } else if (signaling_down) {
            signaling_down = false;
            if (call_inbox) {
                send_replay(call_inbox->take_replay());
            }
            if (redial_needs_fresh_socket && !cleanup_stop_pending &&
                socket_generation > cleanup_generation) {
                if (!send_cleanup_stop()) {
                    begin_fresh_socket_cleanup();
                }
            }
        }
        car_dirty = true;
    };
    while (running && !platform.display->should_close() &&
           (observe_forever || Clock::now() < actor_deadline || (call && call->active()) ||
            write_access.waiting() || call_gate.pending() ||
            passkey.mode() == PasskeyMode::Phone ||
            (draining_cleanup && Clock::now() < cleanup_exit_deadline) ||
            (desktop_ui && (ended_view.phase == CallPhase::Ended ||
                            ended_view.phase == CallPhase::Failed)))) {
        if (upgrade_puck()) {
            car_dirty = true;
        }
        std::string message;
        std::string poll_error;
        while (reader.take(&message, &poll_error)) {
            handle_actor_message(message);
            car_dirty = true;
        }
        if (!poll_error.empty()) {
            log::warn("actor: %s", redact_text(poll_error).c_str());
        }

        // The socket is only ready for a call once the server has answered on
        // this connection. This is deliberately after the frame is queued, so
        // the first handler sees the resume response before any start request.
        on_observer_ready();
        if (attach_pending && Clock::now() >= attach_deadline) {
            attach_pending = false;
            attach_error = "the actor did not acknowledge the voice reattach";
            log::warn("voice: %s", attach_error.c_str());
            if (call) {
                call->fail_recovery("voice reattach timed out");
            }
            car_dirty = true;
        }
        if (cleanup_stop_pending && Clock::now() >= cleanup_deadline) {
            log::warn("voice: cleanup stop receipt timed out; moving to a fresh socket");
            begin_fresh_socket_cleanup();
            car_dirty = true;
        }
        if (draining_cleanup && !redial_needs_fresh_socket && !cleanup_stop_pending) {
            log::info("voice: previous-session cleanup confirmed; leaving the car screen");
            running = false;
            break;
        }
        if (draining_cleanup && Clock::now() >= cleanup_exit_deadline) {
            log::warn("voice: previous-session cleanup did not confirm before the exit bound");
            running = false;
            break;
        }

        if (reader.drained_and_closed()) {
            if (!signaling_down) {
                signaling_down = true;
                log::warn("actor: signaling closed; keeping the current media session");
                if (call_inbox) {
                    call_inbox->begin_replay();
                }
                next_reconnect = Clock::now();
            }
            if (redial_needs_fresh_socket && !fresh_socket_required) {
                begin_fresh_socket_cleanup();
            }
        }
        if (signaling_down && (!websocket || !websocket->is_open()) &&
            Clock::now() >= next_reconnect) {
            const auto now = std::chrono::duration<double>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            request_prepare(token_exp > 0.0 && now >= token_exp - 45.0);
            car_dirty = true;
        }

        ActorPrepareResult prepared;
        if (actor_prepare.take(&prepared)) {
            if (prepared.ok && apply_prepared(prepared)) {
                prepare_wanted = false;
                prepare_refresh = false;
                reconnect_attempts = 0;
            } else {
                ++reconnect_attempts;
                const auto backoff = std::chrono::milliseconds(
                    std::min(5000, 250 * (1 << std::min(reconnect_attempts, 4))));
                next_reconnect = Clock::now() + backoff;
                prepare_wanted = prepare_wanted || prepare_refresh ||
                                 write_access.waiting() || signaling_down;
                log::warn("actor: background preparation failed (%s); retrying in %lld ms",
                          redact_text(prepared.error).c_str(),
                          static_cast<long long>(backoff.count()));
            }
            car_dirty = true;
        }
        if (prepare_wanted && !actor_prepare.busy() && Clock::now() >= next_reconnect) {
            if (prepare_actor(prepare_refresh)) {
                prepare_wanted = false;
            } else {
                next_reconnect = Clock::now() + std::chrono::milliseconds(500);
            }
            car_dirty = true;
        }

        // DataChannel callbacks only enqueue. session.closed, plugin data, and
        // anything else are delivered to the controller on this thread, where
        // its lifetime is stable.
        if (call && call_inbox) {
            std::deque<std::string> data = call_inbox->take_data();
            for (const std::string& text : data) {
                call->on_data_message(text);
            }
        }

        AuthorizationStatus authorization_update;
        while (authorization.take(&authorization_update)) {
            if (!apply_authorization(authorization_update)) {
                running = false;
                break;
            }
            car_dirty = true;
        }
        if (!running) {
            break;
        }
        if (call_gate.update(explicit_write_permission(session) && write_access.write_ready() &&
                                 observer_ready && !redial_needs_fresh_socket,
                             websocket && websocket->is_open() && !signaling_down)) {
            if (!start_call()) {
                log::error("voice: the authorized call request could not start");
            }
            car_dirty = true;
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

        if (Clock::now() - last_renew >= std::chrono::seconds(30) && !media.busy() &&
            !renew_task.busy()) {
            last_renew = Clock::now();
            renew_task.start([&](std::string* renew_error) {
                return pairing.establish_session(renew_error);
            });
        }
        bool renew_ok = false;
        std::string renew_error;
        if (renew_task.take(&renew_ok, &renew_error)) {
            if (!renew_ok) {
                log::warn("car session renew: %s", redact_text(renew_error).c_str());
                if (renew_error.find("401") != std::string::npos) {
                    if (call) {
                        call->end("car session expired");
                    } else if (options.temporary_session) {
                        // An idle shared session that cannot renew must not
                        // leave the previous account on screen.
                        log::info("temporary session expired; returning to pairing");
                        restart_after_leave = true;
                        running = false;
                        break;
                    }
                }
            } else {
                // The session cookie rotates on every renew; keep the stored
                // copy current or the next process resumes with a dead cookie.
                std::string persist_error;
                if (!options.temporary_session &&
                    !pairing.persist(session, &persist_error)) {
                    log::warn("cannot persist renewed session: %s",
                              redact_text(persist_error).c_str());
                }
            }
        }
        const double now = std::chrono::duration<double>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        if (token_exp > 0.0 && now >= token_exp - 45.0 && !media.busy() &&
            !actor_prepare.busy() && !prepare_wanted) {
            // The old socket stays usable while the worker refreshes the token
            // and opens its replacement.
            request_prepare(true);
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
                const bool user_hangup = ended_by_user;
                finish_call_terminal(user_hangup);
                car_dirty = true;
                sync_car();
                render(true);
                if (options.temporary_session) {
                    restart_after_leave = true;
                    running = false;
                    break;
                }
                if (!desktop_ui) {
                    if (redial_needs_fresh_socket) {
                        // Headless still has to finish the old-session cleanup
                        // on a fresh socket before exiting. Keep the reader and
                        // the preparation worker running for a bounded time.
                        draining_cleanup = true;
                        cleanup_exit_deadline = Clock::now() + std::chrono::seconds(10);
                        continue;
                    }
                    // The terminal state is reached and the close sequence has
                    // released the peer.
                    running = false;
                    break;
                }
                // A normal desktop keeps the car screen on a failure or a
                // remote hangup, with the End control as the route back to
                // the green call button.
                continue;
            }
        }
        if (options.temporary_session) {
            if (call) {
                temporary_idle_deadline = Clock::now() + std::chrono::minutes(3);
            } else if (Clock::now() >= temporary_idle_deadline) {
                log::info("temporary session idle for 3 minutes; returning to pairing");
                restart_after_leave = true;
                running = false;
                break;
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
        call->end("leaving the car screen");
        // Window close, CLI exit, and a lost actor all leave through here.
        // Pump the actor and the call state machine for a bounded time so the
        // stop/close handshake can finish instead of dropping the peer early.
        const auto close_deadline = Clock::now() + std::chrono::seconds(5);
        while (!call->ended() && !call->failed() && Clock::now() < close_deadline) {
            std::string message;
            while (reader.take(&message, nullptr)) {
                handle_actor_message(message);
            }
            if (call && call_inbox) {
                std::deque<std::string> data = call_inbox->take_data();
                for (const std::string& text : data) {
                    call->on_data_message(text);
                }
            }
            final_stats = voice_engine->stats();
            call->tick(Clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!call->ended() && !call->failed()) {
            log::warn("voice: close sequence did not complete before the exit bound");
        }
    }
    if (call && (call->ended() || call->failed())) {
        // Use the same terminal decision as the main loop. Display budget
        // exhaustion can end the UI loop while the call is still active, and
        // an unconfirmed stop must still run the fresh-socket cleanup.
        finish_call_terminal(ended_by_user);
    }
    if (call) {
        // The close handshake did not reach ended()/failed() within the bound.
        // Release the controller and treat the close as unconfirmed so the
        // bounded cleanup drain below still runs.
        log::warn("voice: releasing an unfinished call at the exit bound");
        if (voice_engine) {
            final_stats = voice_engine->stats();
        }
        media.close();
        plugins.reset_requests(false);
        call.reset();
        voice_engine.reset();
        call_inbox.reset();
        signaling_down = false;
        attach_pending = false;
        attach_request_id.clear();
        redial_needs_fresh_socket = true;
        cleanup_stop_pending = false;
        cleanup_stop_request_id.clear();
        cleanup_deadline = Clock::time_point{};
        begin_fresh_socket_cleanup();
        ended_by_user = false;
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

    // A window close can land after an unconfirmed call close but before the
    // cleanup stop was retried. Finish that bounded recovery before dropping
    // the actor socket so the next launch does not inherit an unconfirmed
    // session. Headless/CLI normally does this in the main loop.
    if (!call && redial_needs_fresh_socket) {
        log::info("voice: finishing previous-session cleanup before exit");
        const auto exit_cleanup_deadline = Clock::now() + std::chrono::seconds(10);
        while (redial_needs_fresh_socket && Clock::now() < exit_cleanup_deadline) {
            std::string message;
            while (reader.take(&message, nullptr)) {
                handle_actor_message(message);
            }
            on_observer_ready();
            ActorPrepareResult prepared;
            if (actor_prepare.take(&prepared)) {
                if (!prepared.ok || !apply_prepared(prepared)) {
                    prepare_wanted = true;
                }
            }
            if ((prepare_wanted || fresh_socket_required) && !actor_prepare.busy()) {
                request_prepare(false);
            }
            if (cleanup_stop_pending && Clock::now() >= cleanup_deadline) {
                begin_fresh_socket_cleanup();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (redial_needs_fresh_socket) {
            log::warn("voice: previous-session cleanup remained unconfirmed at the exit bound");
        }
    }

    const bool actor_was_open = websocket->is_open();
    const std::string close_reason = websocket->close_reason();
    reader.stop();
    websocket->close();
    if (!close_reason.empty()) {
        log::info("actor: closed (%s)", redact_text(close_reason).c_str());
    }
    log::info("actor: observed %d message(s)", message_count);

    if (options.temporary_session) {
        // Shared kiosk: DELETE the temporary session while the cookie is
        // still present, then erase the saved session and the in-memory jar.
        // Stop every background network worker first so a late renew/token
        // fetch cannot recreate the account after the delete.
        renew_task.stop();
        actor_prepare.stop();
        authorization.stop();
        pairing.stop_polling();
        std::string leave_error;
        pairing.leave_session(&leave_error);
        if (!leave_error.empty()) {
            log::warn("temporary session cleanup: %s", redact_text(leave_error).c_str());
        }
        const bool return_to_qr = restart_after_leave && !quit && !exit_after_script_leave &&
                                  !platform.display->should_close();
        return return_to_qr ? kRestartPairing : 0;
    }

    if (exit_after_script_leave) {
        return 0;
    }
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
    if (options.call && !options.pair && !options.temporary_session) {
        log::error("--call requires --pair (or --temporary-session)");
        return 2;
    }

    std::string error;
    PlatformOptions platform_options = options.platform;
    if (options.demo_call || !options.demo_media.empty()) {
        // The demos are a single frame; a larger counter would suppress the
        // PPM write they exist to produce.
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

    PairingOptions pairing_options;
    // Test hook: point the car/pairing endpoints at a local stand-in during
    // desktop verification. Only loopback origins are accepted so a stray
    // environment variable cannot redirect the session cookie jar.
    if (const char* origin = std::getenv("PUCK_KIOSK_ORIGIN"); origin && *origin) {
        const std::string value = origin;
        static const std::regex loopback_re(
            R"(^http://(?:127\.0\.0\.1|localhost|\[::1\])(?::[0-9]+)?/?$)");
        if (std::regex_match(value, loopback_re)) {
            pairing_options.origin = value;
            log::info("origin override: %s", redact_url(pairing_options.origin).c_str());
        } else {
            log::warn("ignoring non-loopback origin override");
        }
    }

    const bool demo = options.demo_call || !options.demo_media.empty();
    auto puck = demo ? create_puck_view(options.enable_3d) : create_placeholder_puck_view();
    log::info("Puck renderer: %s%s", puck->name(),
              demo ? "" : " (placeholder while the asset cache is verified)");

    CarUi ui(*puck);
    if (platform->display->needs_client_caption()) {
        ui.set_client_caption(true);
        platform->display->set_client_caption(
            ClientCaption{CarUi::kCaptionHeight, CarUi::kCaptionButtonsWidth});
    }
    MediaOverlay media(platform->http.get(), pairing_options.origin, renderer.canvas());
    ui.set_media(&media);

    PluginUi plugins;
    PasskeyFlow passkey;
    Diagnostics diagnostics(platform->http.get(), pairing_options.origin);
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
    AppOptions session_options = options;
    for (;;) {
        const int result = run_session(session_options, *platform, renderer, ui, media, plugins,
                                       passkey, diagnostics, pairing_options, puck);
        if (result != kRestartPairing) {
            return result;
        }
        // Shared-device exit: the previous account and cookie jar are gone.
        // Start a fresh PairingSession on the same window and show its QR.
        // --call is a one-shot test hook; the next driver must press Call.
        session_options.call = false;
        session_options.pair = true;
        media.close();
        plugins.reset_requests(true);
        passkey = PasskeyFlow{};
        diagnostics.set_session(false, {});
        diagnostics.set_call_active(false);
        diagnostics.clear_call_aec_status();
        ui.set_call(CallView{});
        ui.set_pairing_recovery(PairingRecovery::None);
    }
}

}  // namespace pk
