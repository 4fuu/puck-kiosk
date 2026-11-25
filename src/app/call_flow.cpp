#include "app/call_flow.h"

#include "app/pairing.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace pk {
namespace {

using Clock = std::chrono::steady_clock;

// Request ids are unique for the process so a late answer to an old call can
// never match the request a redial sent on the same actor socket.
constexpr int kFirstRequestId = 1000000000;
constexpr auto kDestructorGrace = std::chrono::milliseconds(500);
std::atomic<int> g_next_request_id{kFirstRequestId};

int next_request_id() {
    return g_next_request_id.fetch_add(1);
}

int seconds_since(Clock::time_point from, Clock::time_point now) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - from).count());
}

bool id_matches(const nlohmann::json& id, int expected) {
    if (id.is_number_integer()) {
        return id.get<int>() == expected;
    }
    if (id.is_string()) {
        return id.get<std::string>() == std::to_string(expected);
    }
    return false;
}

std::string error_text(const nlohmann::json& error) {
    if (error.is_string()) {
        return error.get<std::string>();
    }
    return redact_json(error.dump(), 300);
}

bool ready_status(const std::string& status) {
    return status == "listening" || status == "thinking";
}

}  // namespace

void CallSignal::update_output(float level, Clock::time_point now) {
    if (level >= kSpeakingThreshold) {
        output_above_ = true;
        output_last_above_ = now;
    }
}

bool CallSignal::speaking(Clock::time_point now) const {
    return output_above_ && now - output_last_above_ <= kSpeakingHold;
}

void CallSignal::barge_in(Clock::time_point) {
    output_above_ = false;
}

void CallSignal::set_user_speaking(bool speaking, Clock::time_point now) {
    if (speaking) {
        user_speaking_ = true;
        return;
    }
    if (user_speaking_) {
        user_speaking_ = false;
        user_hold_until_ = now + kUserSpeakingHold;
    }
}

bool CallSignal::user_active(Clock::time_point now) const {
    return user_speaking_ || now < user_hold_until_;
}

CallController::CallController(WebSocket& actor, voice::VoiceMedia& engine, CallOptions options)
    : actor_(&actor), engine_(engine), options_(options) {}

CallController::~CallController() {
    bool should_end = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_end = !stopped_ && !stopping_;
    }
    if (should_end) {
        end("shutdown");
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        close_cv_.wait_for(lock, kDestructorGrace,
                           [this] { return stopped_ || remote_closed_; });
    }
    if (!stopped_) {
        finish_shutdown(false);
    } else if (offer_thread_.joinable()) {
        offer_thread_.join();
    }
}

bool CallController::send_json(const nlohmann::json& value) {
    return actor_ && actor_->send_text(value.dump());
}

void CallController::rebind_actor(WebSocket& actor) {
    actor_ = &actor;
}

bool CallController::reattach() {
    int request_id = 0;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || stopping_ || failed_ || !answer_received_ || session_id_.empty()) {
            return false;
        }
        request_id = next_request_id();
        attach_request_id_ = request_id;
        session_id = session_id_;
    }

    nlohmann::json params;
    params["transport"] = {{"type", "webrtc_attach"}, {"sessionID", session_id}};
    params["mediaKinds"] = nlohmann::json::array({"image"});
    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"method", "client_puck_voice_start"},
        {"params", params},
    };
    if (!send_json(request)) {
        log::warn("voice: cannot send the voice reattach request");
        return false;
    }
    log::info("voice: reattach sent for session %s", redact_text(session_id).c_str());
    return true;
}

bool CallController::start(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_called_) {
            if (error) {
                *error = "call already started";
            }
            return false;
        }
        if (!actor_ || !actor_->is_open()) {
            if (error) {
                *error = "actor socket is not open";
            }
            return false;
        }
        start_called_ = true;
        call_started_ = Clock::now();
        offer_deadline_ = call_started_ + std::chrono::seconds(options_.offer_timeout_seconds);
        view_.phase = CallPhase::Starting;
        view_.caption = "Connecting";
        view_.detail.clear();
    }

    std::string media_error;
    if (!engine_.start_audio(&media_error)) {
        const std::string message = "audio unavailable: " + media_error;
        if (error) {
            *error = message;
        }
        fail(message);
        return false;
    }

    try {
        offer_thread_ = std::thread([this] {
            std::string sdp;
            std::string offer_error;
            const bool ok = engine_.create_offer(&sdp, &offer_error);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                offer_done_ = true;
                offer_ok_ = ok;
                offer_sdp_ = std::move(sdp);
                offer_error_ = std::move(offer_error);
            }
        });
    } catch (const std::exception& exception) {
        const std::string message = std::string("cannot start ICE: ") + exception.what();
        if (error) {
            *error = message;
        }
        fail(message);
        return false;
    }
    log::info("voice: microphone and speaker open; ICE gathering in the background");
    return true;
}

void CallController::on_actor_message(const std::string& text) {
    const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded()) {
        return;
    }
    if (message.is_array()) {
        // RivetKit delivers typed event batches as a JSON array in one frame.
        for (const nlohmann::json& event : message) {
            if (event.is_object()) {
                handle_actor_object(event);
            }
        }
        return;
    }
    if (message.is_object()) {
        handle_actor_object(message);
    }
}

void CallController::handle_actor_object(const nlohmann::json& message) {
    if (message.contains("id")) {
        const nlohmann::json& id = message["id"];
        int answer_request = 0;
        int stop_request = 0;
        int attach_request = 0;
        bool closing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            answer_request = answer_request_id_;
            stop_request = stop_request_id_;
            attach_request = attach_request_id_;
            closing = stopping_ || stopped_;
        }
        // A JSON-RPC response must carry jsonrpc "2.0" and no method. The Web
        // response schema allows an omitted result (void success), but an
        // id-only notification or a same-id method call is not a receipt.
        const bool jsonrpc_response =
            message.contains("jsonrpc") && message["jsonrpc"].is_string() &&
            message["jsonrpc"].get<std::string>() == "2.0" &&
            !message.contains("method");
        if (answer_request != 0 && id_matches(id, answer_request)) {
            if (!jsonrpc_response) {
                log::warn("voice: start response has no JSON-RPC response shape; ignored");
                return;
            }
            if (message.contains("error")) {
                if (closing) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        answer_received_ = true;
                        pending_start_ = false;
                        close_cv_.notify_all();
                    }
                    log::info("voice: late start rejection after hangup");
                } else {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        answer_received_ = true;
                    }
                    fail("voice start rejected: " + error_text(message["error"]));
                }
                return;
            }
            if (message.contains("result") && message["result"].is_object()) {
                handle_answer(message["result"]);
            } else if (message.contains("result") && message["result"].is_string()) {
                const nlohmann::json parsed =
                    nlohmann::json::parse(message["result"].get<std::string>(), nullptr, false);
                if (parsed.is_discarded()) {
                    if (closing) {
                        handle_answer(nlohmann::json::object());
                    } else {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            answer_received_ = true;
                        }
                        fail("actor answer is not valid JSON");
                    }
                } else {
                    handle_answer(parsed);
                }
            } else if (message.contains("params") && message["params"].is_object()) {
                handle_answer(message["params"]);
            } else {
                handle_answer(message);
            }
            return;
        }
        if (stop_request != 0 && id_matches(id, stop_request)) {
            if (!jsonrpc_response) {
                log::warn("voice: stop response has no JSON-RPC response shape; ignored");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_pending_ = false;
                stop_ok_ = !message.contains("error");
                close_cv_.notify_all();
            }
            if (message.contains("error")) {
                log::warn("voice: stop rejected: %s", error_text(message["error"]).c_str());
            }
            return;
        }
        if (attach_request != 0 && id_matches(id, attach_request)) {
            if (!jsonrpc_response) {
                log::warn("voice: reattach response has no JSON-RPC response shape; ignored");
                return;
            }
            if (message.contains("error")) {
                log::warn("voice: reattach rejected: %s", error_text(message["error"]).c_str());
            }
            return;
        }
        if (jsonrpc_response && id.is_string() &&
            id.get<std::string>().rfind("puck-kiosk-", 0) == 0 && message.contains("error")) {
            log::warn("voice: client_resume rejected: %s", error_text(message["error"]).c_str());
        }
        // A response with an id we do not own is never interpreted as an
        // uncorrelated notification; that is what lets a redial ignore stale
        // start answers.
        return;
    }

    if (message.contains("method") && message["method"].is_string()) {
        const std::string method = message["method"].get<std::string>();
        const nlohmann::json params = message.value("params", nlohmann::json::object());
        if (method == "puck_voice_state") {
            handle_voice_state(params);
        } else if (method == "puck_voice_transcript") {
            handle_transcript(params);
        } else if (method == "client_puck_voice_start") {
            log::info("voice: uncorrelated start answer ignored");
        }
        return;
    }

    if (message.contains("type") && message["type"].is_string()) {
        const std::string type = message["type"].get<std::string>();
        const nlohmann::json* payload = &message;
        if (message.contains("params") && message["params"].is_object()) {
            payload = &message["params"];
        } else if (message.contains("data") && message["data"].is_object()) {
            payload = &message["data"];
        } else if (message.contains("event") && message["event"].is_object()) {
            payload = &message["event"];
        }
        if (type == "puck_voice_state") {
            handle_voice_state(*payload);
        } else if (type == "puck_voice_transcript") {
            handle_transcript(*payload);
        }
    }
}

void CallController::handle_answer(const nlohmann::json& payload) {
    bool closing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing = stopping_ || stopped_;
    }

    const auto compensate_late_start = [this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            answer_received_ = true;
            pending_start_ = false;
            close_cv_.notify_all();
        }
        send_stop_request();
        log::info("voice: late start success after hangup; compensating stop sent");
    };

    if (!payload.is_object()) {
        if (closing) {
            compensate_late_start();
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                answer_received_ = true;
            }
            fail("actor answer is not an object");
        }
        return;
    }
    std::string sdp;
    if (payload.contains("transport") && payload["transport"].is_object()) {
        const nlohmann::json& transport = payload["transport"];
        if (transport.contains("sdp") && transport["sdp"].is_string()) {
            sdp = transport["sdp"].get<std::string>();
        }
    }
    if (sdp.empty()) {
        if (closing) {
            compensate_late_start();
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                answer_received_ = true;
            }
            fail("actor answer has no transport SDP");
        }
        return;
    }

    std::string session_id;
    if (payload.contains("sessionID") && payload["sessionID"].is_string()) {
        session_id = payload["sessionID"].get<std::string>();
    }

    bool late = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!start_request_sent_) {
            log::warn("voice: unsolicited answer ignored");
            return;
        }
        if (answer_received_) {
            return;
        }
        answer_received_ = true;
        pending_start_ = false;
        if (!session_id.empty()) {
            session_id_ = session_id;
        }
        late = stopping_ || stopped_;
        if (!late) {
            connect_deadline_ =
                Clock::now() + std::chrono::seconds(options_.connect_timeout_seconds);
            view_.phase = CallPhase::Connecting;
            view_.caption = "Connecting";
        }
        close_cv_.notify_all();
    }

    if (late) {
        send_stop_request();
        log::info("voice: late answer after hangup (session %s); stop sent",
                  redact_text(session_id).c_str());
        return;
    }

    std::string error;
    if (!engine_.accept_answer(sdp, &error)) {
        fail("cannot apply answer: " + error);
        return;
    }
    // Capture and playback were opened by the preflight before the offer.
    log::info("voice: answer applied (session %s, %zu bytes)", redact_text(session_id).c_str(),
              sdp.size());
}

void CallController::handle_voice_state(const nlohmann::json& params) {
    if (!params.is_object()) {
        return;
    }
    const Clock::time_point now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || stopped_ || remote_closed_ || remote_failed_) {
        return;
    }
    // Uncorrelated state can be an observer echo from the previous call. Only
    // a state observed after this call's start request belongs to this round.
    if (!start_request_sent_) {
        return;
    }

    const bool has_status = params.contains("status") && params["status"].is_string();
    const bool has_user = params.contains("userSpeaking") &&
                          params["userSpeaking"].is_boolean();
    const bool has_puck = params.contains("puckSpeaking") &&
                          params["puckSpeaking"].is_boolean();
    const bool complete = has_status && has_user && has_puck;

    std::string status;
    if (has_status) {
        status = params["status"].get<std::string>();
        if (status != remote_status_) {
            remote_status_ = status;
            log::info("voice: actor state %s", status.c_str());
        }
        if (ready_status(status)) {
            model_ready_ = true;
            chime_eligible_ = true;
        }
        thinking_ = status == "thinking";
        if (status == "failed" || status == "error") {
            remote_failed_ = true;
        } else if (status == "ended" || status == "closed") {
            remote_closed_ = true;
        }
    }

    const bool terminal =
        (has_status && (status == "failed" || status == "error" || status == "ended" ||
                        status == "closed")) ||
        (params.contains("error") && !params["error"].is_null());

    if (has_user) {
        const bool speaking = params["userSpeaking"].get<bool>();
        signal_.set_user_speaking(speaking, now);
        if (speaking) {
            signal_.barge_in(now);
            view_.speaking = false;
            view_.mouth = 0.0f;
            view_.output_level = 0.0f;
            view_.thinking = false;
        }
    }

    // A terminal snapshot ends the call on the next tick. It must not reopen a
    // dictation-paused transmit gate while the audio teardown is still pending.
    if (!terminal) {
        bool paused = dictation_paused_;
        bool paused_defined = false;
        if (params.contains("inputPausedForDictation")) {
            if (params["inputPausedForDictation"].is_boolean()) {
                paused = params["inputPausedForDictation"].get<bool>();
                paused_defined = true;
            }
        } else if (complete) {
            paused = false;
            paused_defined = true;
        }
        if (paused_defined && paused != dictation_paused_) {
            dictation_paused_ = paused;
            view_.dictation_paused = paused;
            apply_input_gate();
        }
    }

    if (params.contains("error") && !params["error"].is_null()) {
        last_error_ = error_text(params["error"]);
        remote_failed_ = true;
    }

    if (remote_closed_) {
        signal_.set_user_speaking(false, now);
    }
}

void CallController::handle_transcript(const nlohmann::json& params) {
    if (!params.is_object()) {
        return;
    }
    std::string text;
    for (const char* key : {"text", "caption", "transcript", "delta"}) {
        if (params.contains(key) && params[key].is_string()) {
            text = params[key].get<std::string>();
            break;
        }
    }
    if (text.empty() && params.contains("entry") && params["entry"].is_object()) {
        const nlohmann::json& entry = params["entry"];
        for (const char* key : {"text", "caption", "transcript"}) {
            if (entry.contains(key) && entry[key].is_string()) {
                text = entry[key].get<std::string>();
                break;
            }
        }
    }
    if (text.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || stopped_) {
        return;
    }
    transcript_ = text;
    view_.detail = text;
    log::info("voice: caption %s", redact_text(text, 160).c_str());
}

void CallController::on_data_message(const std::string& text) {
    const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        return;
    }
    if (!message.contains("type") || !message["type"].is_string() ||
        message["type"].get<std::string>() != "session.closed") {
        return;
    }
    std::string reason;
    if (message.contains("reason") && message["reason"].is_string()) {
        reason = message["reason"].get<std::string>();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remote_closed_ = true;
        if (!reason.empty()) {
            remote_close_reason_ = reason;
        }
        close_cv_.notify_all();
    }
    log::info("voice: data channel session.closed%s%s", reason.empty() ? "" : ": ",
              reason.c_str());
}

void CallController::tick(std::chrono::steady_clock::time_point now) {
    if (offer_thread_.joinable()) {
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done = offer_done_;
        }
        if (done) {
            offer_thread_.join();
        }
    }

    const voice::VoiceEngineStats stats = engine_.stats();
    const voice::PeerState peer_state = engine_.peer_state();
    const bool peer_connected =
        stats.connected || peer_state == voice::PeerState::Connected;

    bool send_start = false;
    std::string start_sdp;
    int start_request_id = 0;
    bool do_mute = false;
    bool do_end = false;
    bool do_fail = false;
    bool finish_close = false;
    bool finish_confirmed = false;
    bool play_chime = false;
    std::string reason;
    std::string error;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        if (stopping_) {
            const bool offer_running = offer_thread_.joinable() && !offer_done_;
            if (!offer_running) {
                const bool peer_gone = peer_state == voice::PeerState::Failed ||
                                       peer_state == voice::PeerState::Closed;
                const bool waiting_close =
                    session_close_sent_ && !remote_closed_ && !peer_gone;
                const bool waiting = pending_start_ || stop_pending_ || waiting_close;
                const bool session_ok =
                    !session_close_sent_ || remote_closed_ || peer_gone;
                if (!waiting) {
                    finish_close = true;
                    finish_confirmed = !pending_start_ && !stop_pending_ && stop_ok_ &&
                                       session_ok;
                } else if (now >= close_deadline_) {
                    finish_close = true;
                    finish_confirmed = false;
                    if (pending_start_) {
                        log::warn("voice: pending start unresolved at close timeout; "
                                  "cleanup is not confirmed");
                    }
                    if (stop_pending_) {
                        log::warn("voice: stop receipt missing at close timeout; "
                                  "redial ordering is not confirmed");
                    }
                    if (waiting_close) {
                        log::warn("voice: session.closed not received before the close timeout");
                    }
                }
            }
            if (!finish_close) {
                return;
            }
        } else {
            if (!start_called_) {
                return;
            }

            if (offer_done_ && !start_request_sent_) {
                if (!offer_ok_) {
                    do_fail = true;
                    error = "cannot create voice offer";
                    if (!offer_error_.empty()) {
                        error += ": " + offer_error_;
                    }
                } else if (view_.phase == CallPhase::Starting) {
                    start_request_id = next_request_id();
                    answer_request_id_ = start_request_id;
                    start_sdp = offer_sdp_;
                    start_request_sent_ = true;
                    answer_deadline_ =
                        now + std::chrono::seconds(options_.answer_timeout_seconds);
                    send_start = true;
                }
            }

            if (!send_start) {
                if (!do_fail && !offer_done_ && now >= offer_deadline_) {
                    do_fail = true;
                    error = "ICE gathering timeout";
                }

                if (!do_fail && remote_closed_) {
                    do_end = true;
                    reason = remote_close_reason_.empty() ? "session closed"
                                                          : remote_close_reason_;
                } else if (!do_fail && remote_failed_) {
                    do_fail = true;
                    error = last_error_.empty() ? "voice session failed" : last_error_;
                } else if (!do_fail &&
                           (peer_state == voice::PeerState::Failed ||
                            (peer_state == voice::PeerState::Closed && answer_received_))) {
                    do_fail = true;
                    error = "WebRTC connection lost";
                } else if (!do_fail && start_request_sent_ && !answer_received_ &&
                           now >= answer_deadline_) {
                    do_fail = true;
                    error = "voice answer timeout";
                } else if (!do_fail && answer_received_ && !peer_connected &&
                           !ever_connected_ && now >= connect_deadline_) {
                    do_fail = true;
                    error = "WebRTC connect timeout";
                } else if (!do_fail && peer_connected && !model_ready_) {
                    if (!model_ready_deadline_set_) {
                        model_ready_deadline_ =
                            now + std::chrono::seconds(options_.model_ready_timeout_seconds);
                        model_ready_deadline_set_ = true;
                    } else if (now >= model_ready_deadline_) {
                        do_fail = true;
                        error = "voice model did not become ready";
                    }
                } else if (model_ready_) {
                    model_ready_deadline_set_ = false;
                }

                if (!do_end && !do_fail) {
                    if (peer_connected) {
                        ever_connected_ = true;
                    }
                    const bool live = peer_connected && model_ready_;
                    view_.peer_connected = peer_connected;
                    view_.ready = live;
                    view_.muted = muted_;
                    view_.dictation_paused = dictation_paused_;
                    view_.closing = false;
                    view_.input_silenced = input_silenced_;

                    if (live && chime_eligible_ && !chime_done_) {
                        chime_done_ = true;
                        play_chime = true;
                    }

                    if (live) {
                        if (view_.phase == CallPhase::Starting ||
                            view_.phase == CallPhase::Connecting) {
                            view_.phase = CallPhase::Active;
                        }
                        if (!signal_.user_talking()) {
                            signal_.update_output(stats.output_level, now);
                        }
                        view_.speaking =
                            !signal_.user_talking() && signal_.speaking(now);
                        view_.user_speaking = signal_.user_active(now);
                        view_.thinking = thinking_ && !view_.user_speaking;
                        view_.input_level = input_silenced_ ? 0.0f : stats.input_level;
                        view_.output_level =
                            signal_.user_talking() ? 0.0f : stats.output_level;
                        view_.mouth = view_.speaking ? stats.output_mouth : 0.0f;
                    } else {
                        signal_.barge_in(now);
                        view_.speaking = false;
                        view_.thinking = false;
                        view_.input_level = 0.0f;
                        view_.output_level = 0.0f;
                        view_.mouth = 0.0f;
                        view_.user_speaking = false;
                    }
                    view_.jitter_ms = stats.jitter_target_frames * 20;

                    if (view_.phase != CallPhase::Starting &&
                        view_.phase != CallPhase::Connecting) {
                        if (view_.speaking) {
                            view_.phase = CallPhase::Speaking;
                        } else if (muted_) {
                            view_.phase = CallPhase::Muted;
                        } else if (view_.thinking) {
                            view_.phase = CallPhase::Thinking;
                        } else {
                            view_.phase = CallPhase::Active;
                        }
                    }

                    switch (view_.phase) {
                        case CallPhase::Idle:
                            view_.caption = "Call Puck";
                            break;
                        case CallPhase::Starting:
                            view_.caption = "Connecting";
                            break;
                        case CallPhase::Connecting:
                            view_.caption =
                                peer_connected ? "Waiting for Puck" : "Connecting";
                            break;
                        case CallPhase::Active:
                            view_.caption = dictation_paused_ && !muted_
                                                ? "Microphone paused"
                                                : "On a call with Puck";
                            break;
                        case CallPhase::Speaking:
                            view_.caption = "Puck is speaking";
                            break;
                        case CallPhase::Thinking:
                            view_.caption = "Puck is thinking";
                            break;
                        case CallPhase::Muted:
                            view_.caption = "Microphone off";
                            break;
                        case CallPhase::Ended:
                            view_.caption = "Call Ended";
                            break;
                        case CallPhase::Failed:
                            view_.caption = "Call failed";
                            break;
                    }
                    view_.detail = transcript_;

                    const int elapsed = seconds_since(call_started_, now);
                    if (!scheduled_mute_ && options_.mute_after_seconds >= 0 &&
                        elapsed >= options_.mute_after_seconds) {
                        scheduled_mute_ = true;
                        do_mute = true;
                    }
                    if (!scheduled_end_ && options_.end_after_seconds >= 0 &&
                        elapsed >= options_.end_after_seconds) {
                        scheduled_end_ = true;
                        do_end = true;
                        reason = "scheduled end";
                    }
                }
            }
        }
    }

    if (finish_close) {
        finish_shutdown(finish_confirmed);
        return;
    }

    if (send_start) {
        nlohmann::json params;
        params["transport"] = {{"type", "webrtc"}, {"sdp", start_sdp}};
        params["mediaKinds"] = nlohmann::json::array({"image"});
        nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", start_request_id},
            {"method", "client_puck_voice_start"},
            {"params", params},
        };
        if (!send_json(request)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                start_request_sent_ = false;
                answer_request_id_ = 0;
            }
            fail("cannot send voice offer");
            return;
        }
        log::info("voice: client_puck_voice_start sent (request %d, offer %zu bytes)",
                  start_request_id, start_sdp.size());
    }

    if (play_chime) {
        engine_.play_ready_chime();
    }
    if (do_mute) {
        mute(true);
    }
    if (do_fail) {
        fail(error);
    } else if (do_end) {
        end(reason);
    }
}

void CallController::mute(bool muted) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || stopped_ || muted_ == muted) {
            return;
        }
        muted_ = muted;
        view_.muted = muted;
        apply_input_gate();
        notify = start_request_sent_ && !stopping_;
    }

    if (notify) {
        nlohmann::json notification = {
            {"jsonrpc", "2.0"},
            {"method", "client_puck_voice_user_state"},
            {"params", {{"userState", {{"muted", muted}}}}},
        };
        if (!send_json(notification)) {
            log::warn("voice: cannot send mute state to the actor");
        }
    }
    log::info("voice: %s", muted ? "muted" : "unmuted");
}

void CallController::apply_input_gate() {
    const bool silenced = muted_ || dictation_paused_ || stopping_;
    view_.input_silenced = silenced;
    if (silenced) {
        view_.input_level = 0.0f;
    }
    if (silenced == input_silenced_) {
        return;
    }
    input_silenced_ = silenced;
    engine_.set_input_silenced(silenced);
}

bool CallController::send_stop_request() {
    int request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_id = next_request_id();
        stop_request_id_ = request_id;
        stop_pending_ = false;
        stop_ok_ = false;
    }

    const nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"method", "client_puck_voice_stop"},
        {"params", nlohmann::json::object()},
    };
    if (!send_json(request)) {
        log::warn("voice: cannot send client_puck_voice_stop");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_pending_ = true;
        close_cv_.notify_all();
    }
    return true;
}

void CallController::begin_shutdown(bool failure, const std::string& reason) {
    bool need_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || stopped_) {
            return;
        }
        stopping_ = true;
        if (failure) {
            failed_ = true;
            last_error_ = reason;
        } else {
            ended_ = true;
        }
        view_.phase = failure ? CallPhase::Failed : CallPhase::Ended;
        view_.caption = failure ? "Call failed" : "Call Ended";
        view_.detail = reason;
        view_.closing = true;
        view_.close_confirmed = false;
        view_.peer_connected = false;
        view_.ready = false;
        view_.user_speaking = false;
        view_.speaking = false;
        view_.thinking = false;
        view_.input_level = 0.0f;
        view_.output_level = 0.0f;
        view_.mouth = 0.0f;
        input_silenced_ = true;
        view_.input_silenced = true;
        close_deadline_ =
            Clock::now() + std::chrono::seconds(options_.close_timeout_seconds);
        pending_start_ = start_request_sent_ && !answer_received_;
        stop_pending_ = false;
        stop_ok_ = !start_request_sent_;
        need_stop = start_request_sent_;
    }

    engine_.set_input_silenced(true);
    engine_.stop_audio();

    if (need_stop) {
        const bool close_sent = engine_.send_data("{\"type\":\"session.close\"}");
        if (close_sent) {
            std::lock_guard<std::mutex> lock(mutex_);
            session_close_sent_ = true;
        }
        send_stop_request();
    }

    bool finish_now = false;
    bool finish_confirmed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool offer_running = offer_thread_.joinable() && !offer_done_;
        const voice::PeerState peer_state = engine_.peer_state();
        const bool peer_gone = peer_state == voice::PeerState::Failed ||
                               peer_state == voice::PeerState::Closed;
        const bool waiting_close =
            session_close_sent_ && !remote_closed_ && !peer_gone;
        const bool session_ok = !session_close_sent_ || remote_closed_ || peer_gone;
        finish_now = !offer_running && !pending_start_ && !stop_pending_ &&
                     !waiting_close;
        finish_confirmed = !pending_start_ && !stop_pending_ && stop_ok_ &&
                           session_ok;
    }

    if (failure) {
        log::error("voice: %s", reason.c_str());
    } else {
        log::info("voice: call ended (%s)", reason.c_str());
    }
    if (finish_now) {
        finish_shutdown(finish_confirmed);
    } else {
        log::info("voice: waiting up to %d s for close confirmations",
                  options_.close_timeout_seconds);
    }
}

void CallController::finish_shutdown(bool confirmed) {
    if (offer_thread_.joinable()) {
        offer_thread_.join();
    }
    engine_.set_input_silenced(true);
    engine_.stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        pending_start_ = false;
        stop_pending_ = false;
        view_.closing = false;
        view_.close_confirmed = confirmed;
        view_.peer_connected = false;
        view_.ready = false;
        view_.user_speaking = false;
        view_.speaking = false;
        view_.thinking = false;
        view_.input_level = 0.0f;
        view_.output_level = 0.0f;
        view_.mouth = 0.0f;
        view_.input_silenced = true;
        close_cv_.notify_all();
    }
    log::info("voice: peer released (%s)",
              confirmed ? "close confirmed" : "close not confirmed");
}

void CallController::end(const std::string& reason) {
    begin_shutdown(false, reason);
}

void CallController::fail_recovery(const std::string& error) {
    begin_shutdown(true, error);
}

void CallController::fail(const std::string& error) {
    begin_shutdown(true, error);
}

CallView CallController::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}

PuckVisual CallController::visual() const {
    const CallView current = view();
    PuckVisual visual;
    visual.speaking = current.speaking;
    visual.listening = current.in_call() && current.ready && !current.muted &&
                       !current.dictation_paused && current.user_speaking &&
                       !current.closing;
    visual.thinking = current.thinking;
    visual.not_listening = current.muted || current.dictation_paused || current.closing;
    visual.mouth = current.mouth;
    return visual;
}

bool CallController::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_.in_call();
}

bool CallController::ended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_ && stopped_;
}

bool CallController::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_ && stopped_;
}

std::string CallController::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string CallController::session_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_id_;
}

}  // namespace pk
