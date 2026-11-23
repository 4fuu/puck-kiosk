#include "app/app.h"

#include "app/car_ui.h"
#include "app/pairing.h"
#include "core/log.h"
#include "puck/puck_view.h"
#include "render/renderer.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace pk {
namespace {

using Clock = std::chrono::steady_clock;

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

int run_pairing(const AppOptions& options, Platform& platform, Renderer& renderer, CarUi& ui) {
    if (!platform.http) {
        log::error("pairing requires an HTTP client");
        return 1;
    }
    if (!platform.display) {
        log::error("pairing requires a display");
        return 1;
    }

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
    auto render = [&]() {
        platform.display->make_current();
        renderer.begin(platform.display->size(), platform.display->dpi_scale());
        const float elapsed = std::chrono::duration<float>(Clock::now() - started).count();
        ui.draw(renderer.canvas(), platform.display->size(), elapsed);
        renderer.end();
        platform.display->swap();
    };

    auto show_challenge = [&]() {
        ui.set_pairing(pairing.qr(), "Call Puck", "Waiting for scan");
        render();
        std::cout << pairing.pairing_url() << std::endl;
        log::info("pairing QR written to %s", options.platform.output_path.c_str());
    };

    if (!resumed) {
        show_challenge();
    }
    int generation = pairing.challenge_generation();
    std::string shown_code;

    const auto deadline = Clock::now() + std::chrono::seconds(options.pair_timeout_seconds);
    PairingStatus status;
    if (resumed) {
        status.state = PairingState::Paired;
    }
    PairingState last_state = PairingState::Idle;
    while (!resumed && Clock::now() < deadline) {
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
        }
        if (status.state == PairingState::Scanned && !status.code.empty() &&
            status.code != shown_code) {
            shown_code = status.code;
            ui.set_pairing(pairing.qr(), "Confirm on Your Device", "Confirm on your device",
                           status.code);
            render();
            log::info("pairing scanned; code %s", status.code.c_str());
        }
        if (status.state == PairingState::Paired) {
            break;
        }
        if (status.state == PairingState::Failed || status.state == PairingState::Expired) {
            log::error("pairing failed: %s", status.message.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(pairing.poll_interval_seconds()));
    }

    if (status.state != PairingState::Paired) {
        log::error("pairing did not complete within %d seconds", options.pair_timeout_seconds);
        return 1;
    }

    ui.set_pairing(pairing.qr(), "Connecting", "Paired. Starting session...");
    render();

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

    ActorEndpoint endpoint;
    if (!build_actor_endpoint(session, page_load_id, &endpoint, &error)) {
        log::error("actor endpoint: %s", error.c_str());
        return 2;
    }

    log::info("actor: connecting to %s", endpoint.display_url.c_str());
    const std::vector<HttpHeader> headers = {
        {"Origin", pairing.origin()},
        {"User-Agent", "puck-kiosk/0.1"},
    };
    auto websocket =
        platform.websockets->connect(endpoint.url, headers, endpoint.subprotocols, &error);
    if (!websocket) {
        log::error("actor connect: %s", redact_text(error).c_str());
        return 2;
    }

    log::info("actor: connected; observing for %d seconds", options.actor_seconds);
    int message_count = 0;
    auto last_ping = Clock::now();
    auto last_renew = Clock::now();
    const auto actor_deadline = Clock::now() + std::chrono::seconds(options.actor_seconds);
    while (Clock::now() < actor_deadline) {
        if (Clock::now() - last_renew >= std::chrono::seconds(30)) {
            last_renew = Clock::now();
            std::string renew_error;
            if (!pairing.establish_session(&renew_error)) {
                log::warn("car session renew: %s", redact_text(renew_error).c_str());
            }
        }
        if (websocket->is_open() && Clock::now() - last_ping >= std::chrono::seconds(30)) {
            websocket->send_text("ping");
            last_ping = Clock::now();
        }
        std::string message;
        std::string poll_error;
        if (websocket->poll(message, &poll_error)) {
            if (message == "pong") {
                continue;
            }
            ++message_count;
            log_actor_message(message);
            continue;
        }
        if (!websocket->is_open()) {
            break;
        }
        if (!poll_error.empty()) {
            log::warn("actor: %s", redact_text(poll_error).c_str());
        }
    }
    const bool actor_was_open = websocket->is_open();
    const std::string close_reason = websocket->close_reason();
    websocket->close();
    if (!close_reason.empty()) {
        log::info("actor: closed (%s)", redact_text(close_reason).c_str());
    }
    log::info("actor: observed %d message(s)", message_count);
    return actor_was_open ? 0 : 2;
}

}  // namespace

int App::run(const AppOptions& options) {
    std::string error;
    PlatformOptions platform_options = options.platform;
    if (options.pair) {
        // Pairing writes a snapshot after each state change; a frame counter
        // larger than one would suppress those writes on the headless backend.
        platform_options.frames = 1;
    }
    auto platform = create_platform(platform_options, &error);
    if (!platform) {
        log::error("%s", error.c_str());
        return 1;
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
    if (options.pair) {
        return run_pairing(options, *platform, renderer, ui);
    }

    const auto started = std::chrono::steady_clock::now();
    bool running = true;

    while (running && !platform->display->should_close()) {
        InputEvent event;
        while (platform->input && platform->input->poll(event)) {
            if (event.type == InputEvent::Type::Quit) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        platform->display->make_current();
        renderer.begin(platform->display->size(), platform->display->dpi_scale());
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        ui.draw(renderer.canvas(), platform->display->size(), elapsed);
        renderer.end();
        platform->display->swap();
    }

    return 0;
}

}  // namespace pk
