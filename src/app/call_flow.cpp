#include "app/call_flow.h"

#include "app/pairing.h"
#include "core/log.h"

#include <algorithm>

namespace pk {
namespace {

using Clock = std::chrono::steady_clock;

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

}  // namespace

CallController::CallController(WebSocket& actor, voice::VoiceEngine& engine, CallOptions options)
    : actor_(&actor), engine_(engine), options_(options) {}

CallController::~CallController() {
    bool should_end = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_end = !ended_ && !failed_;
    }
    if (should_end) {
        end("shutdown");
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
        if (ended_ || failed_ || !answer_received_ || session_id_.empty()) {
            return false;
        }
        request_id = ++request_id_;
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
        if (start_sent_) {
            if (error) {
                *error = "call already started";
            }
            return false;
        }
    }

    std::string sdp;
    if (!engine_.create_offer(&sdp, error)) {
        fail(error ? *error : "cannot create voice offer");
        return false;
    }

    int request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_id = ++request_id_;
        answer_request_id_ = request_id;
        start_sent_ = true;
        call_started_ = Clock::now();
        answer_deadline_ = call_started_ + std::chrono::seconds(options_.answer_timeout_seconds);
        view_.phase = CallPhase::Starting;
        view_.caption = "Connecting";
        view_.detail.clear();
    }

    nlohmann::json params;
    params["transport"] = {{"type", "webrtc"}, {"sdp", sdp}};
    params["mediaKinds"] = nlohmann::json::array({"image"});
    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"method", "client_puck_voice_start"},
        {"params", params},
    };
    if (!send_json(request)) {
        if (error) {
            *error = "actor socket is not open";
        }
        fail("cannot send voice offer");
        return false;
    }
    log::info("voice: client_puck_voice_start sent (request %d, offer %zu bytes)", request_id,
              sdp.size());
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
        if (id_matches(id, answer_request_id_)) {
            if (message.contains("error")) {
                fail("voice start rejected: " + error_text(message["error"]));
                return;
            }
            if (message.contains("result") && message["result"].is_object()) {
                handle_answer(message["result"]);
            } else if (message.contains("result") && message["result"].is_string()) {
                const nlohmann::json parsed =
                    nlohmann::json::parse(message["result"].get<std::string>(), nullptr, false);
                if (parsed.is_discarded()) {
                    fail("actor answer is not valid JSON");
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
        if (id_matches(id, stop_request_id_)) {
            return;
        }
        if (id_matches(id, attach_request_id_)) {
            if (message.contains("error")) {
                log::warn("voice: reattach rejected: %s", error_text(message["error"]).c_str());
            }
            return;
        }
        if (id.is_string() && id.get<std::string>().rfind("puck-kiosk-", 0) == 0 &&
            message.contains("error")) {
            log::warn("voice: client_resume rejected: %s", error_text(message["error"]).c_str());
        }
    }

    if (message.contains("method") && message["method"].is_string()) {
        const std::string method = message["method"].get<std::string>();
        const nlohmann::json params = message.value("params", nlohmann::json::object());
        if (method == "puck_voice_state") {
            handle_voice_state(params);
        } else if (method == "puck_voice_transcript") {
            handle_transcript(params);
        } else if (method == "client_puck_voice_start" && params.is_object()) {
            handle_answer(params);
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
    if (!payload.is_object()) {
        fail("actor answer is not an object");
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
        fail("actor answer has no transport SDP");
        return;
    }

    std::string session_id;
    if (payload.contains("sessionID") && payload["sessionID"].is_string()) {
        session_id = payload["sessionID"].get<std::string>();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (answer_received_ || ended_ || failed_) {
            return;
        }
        answer_received_ = true;
        session_id_ = session_id;
        connect_deadline_ = Clock::now() + std::chrono::seconds(options_.connect_timeout_seconds);
        view_.phase = CallPhase::Connecting;
        view_.caption = "Connecting";
    }

    std::string error;
    if (!engine_.accept_answer(sdp, &error)) {
        fail("cannot apply answer: " + error);
        return;
    }
    if (!engine_.start_audio(&error)) {
        fail("cannot start voice audio: " + error);
        return;
    }

    log::info("voice: answer applied (session %s, %zu bytes)", redact_text(session_id).c_str(),
              sdp.size());
}

void CallController::handle_voice_state(const nlohmann::json& params) {
    if (!params.is_object()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (params.contains("status") && params["status"].is_string()) {
        const std::string status = params["status"].get<std::string>();
        if (status != remote_status_) {
            remote_status_ = status;
            log::info("voice: actor state %s", status.c_str());
        }
    }
    if (params.contains("puckSpeaking") && params["puckSpeaking"].is_boolean()) {
        puck_speaking_ = params["puckSpeaking"].get<bool>();
    }
    if (params.contains("userSpeaking") && params["userSpeaking"].is_boolean()) {
        user_speaking_ = params["userSpeaking"].get<bool>();
    }
    if (params.contains("muted") && params["muted"].is_boolean()) {
        const bool actor_muted = params["muted"].get<bool>();
        if (actor_muted != muted_) {
            muted_ = actor_muted;
            view_.muted = actor_muted;
            engine_.set_muted(actor_muted);
        }
    }
    if (params.contains("error") && !params["error"].is_null()) {
        last_error_ = error_text(params["error"]);
        remote_failed_ = true;
    }

    if (remote_status_ == "thinking") {
        thinking_ = true;
    } else if (remote_status_ == "speaking") {
        thinking_ = false;
        puck_speaking_ = true;
    } else if (remote_status_ == "failed" || remote_status_ == "error") {
        remote_failed_ = true;
    } else if (remote_status_ == "ended" || remote_status_ == "closed") {
        remote_closed_ = true;
    } else if (!remote_status_.empty()) {
        thinking_ = false;
        if (remote_status_ == "listening" || remote_status_ == "on-call" ||
            remote_status_ == "not-listening") {
            puck_speaking_ = false;
        }
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
    std::lock_guard<std::mutex> lock(mutex_);
    remote_closed_ = true;
    if (message.contains("reason") && message["reason"].is_string()) {
        remote_close_reason_ = message["reason"].get<std::string>();
    }
    log::info("voice: data channel session.closed%s%s", remote_close_reason_.empty() ? "" : ": ",
              remote_close_reason_.c_str());
}

void CallController::tick(std::chrono::steady_clock::time_point now) {
    bool do_mute = false;
    bool do_end = false;
    bool do_fail = false;
    std::string reason;
    std::string error;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_ || failed_) {
            return;
        }

        const voice::PeerState peer_state = engine_.peer_state();
        if (remote_closed_) {
            do_end = true;
            reason = remote_close_reason_.empty() ? "session closed" : remote_close_reason_;
        } else if (remote_failed_) {
            do_fail = true;
            error = last_error_.empty() ? "voice session failed" : last_error_;
        } else if (view_.phase != CallPhase::Starting && view_.phase != CallPhase::Connecting &&
                   (peer_state == voice::PeerState::Failed ||
                    peer_state == voice::PeerState::Closed)) {
            do_fail = true;
            error = "WebRTC connection lost";
        } else if (view_.phase == CallPhase::Starting && now >= answer_deadline_) {
            do_fail = true;
            error = "voice answer timeout";
        } else if (view_.phase == CallPhase::Connecting) {
            if (engine_.connected()) {
                view_.phase = CallPhase::Active;
            } else if (now >= connect_deadline_) {
                do_fail = true;
                error = "WebRTC connect timeout";
            }
        }

        if (!do_end && !do_fail) {
            const voice::VoiceEngineStats stats = engine_.stats();
            view_.input_level = stats.input_level;
            view_.output_level = stats.output_level;
            view_.mouth = stats.output_mouth;
            view_.jitter_ms = stats.jitter_target_frames * 20;
            const bool speaking = puck_speaking_ || stats.output_level > 0.03f;
            view_.speaking = speaking;
            view_.thinking = thinking_;

            if (view_.phase == CallPhase::Active || view_.phase == CallPhase::Speaking ||
                view_.phase == CallPhase::Thinking || view_.phase == CallPhase::Muted) {
                if (muted_) {
                    view_.phase = CallPhase::Muted;
                } else if (thinking_) {
                    view_.phase = CallPhase::Thinking;
                } else if (speaking) {
                    view_.phase = CallPhase::Speaking;
                } else {
                    view_.phase = CallPhase::Active;
                }
            }

            switch (view_.phase) {
                case CallPhase::Idle:
                    view_.caption = "Call Puck";
                    break;
                case CallPhase::Starting:
                case CallPhase::Connecting:
                    view_.caption = "Connecting";
                    break;
                case CallPhase::Active:
                    view_.caption = "On a call with Puck";
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (muted_ == muted) {
            return;
        }
        muted_ = muted;
        view_.muted = muted;
        if (view_.in_call()) {
            view_.phase = muted ? CallPhase::Muted : CallPhase::Active;
        }
    }
    engine_.set_muted(muted);

    nlohmann::json notification = {
        {"jsonrpc", "2.0"},
        {"method", "client_puck_voice_user_state"},
        {"params", {{"userState", {{"muted", muted}}}}},
    };
    if (!send_json(notification)) {
        log::warn("voice: cannot send mute state to the actor");
    }
    log::info("voice: %s", muted ? "muted" : "unmuted");
}

void CallController::end(const std::string& reason) {
    int request_id = 0;
    bool send_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_ || failed_) {
            return;
        }
        ended_ = true;
        view_.phase = CallPhase::Ended;
        view_.caption = "Call Ended";
        view_.detail = reason;
        request_id = ++request_id_;
        stop_request_id_ = request_id;
        send_stop = start_sent_ && !stop_sent_;
        stop_sent_ = true;
    }

    if (send_stop) {
        // The web client closes the media session over the data channel before
        // sending the JSON-RPC stop.
        engine_.send_data("{\"type\":\"session.close\"}");
    }
    engine_.stop();
    if (send_stop) {
        nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", request_id},
            {"method", "client_puck_voice_stop"},
            {"params", nlohmann::json::object()},
        };
        if (!send_json(request)) {
            log::warn("voice: cannot send client_puck_voice_stop");
        }
    }
    log::info("voice: call ended (%s)", reason.c_str());
}

void CallController::fail(const std::string& error) {
    bool send_stop = false;
    int request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_ || failed_) {
            return;
        }
        failed_ = true;
        view_.phase = CallPhase::Failed;
        view_.caption = "Call failed";
        view_.detail = error;
        last_error_ = error;
        request_id = ++request_id_;
        stop_request_id_ = request_id;
        send_stop = start_sent_ && !stop_sent_;
        stop_sent_ = true;
    }

    if (send_stop) {
        engine_.send_data("{\"type\":\"session.close\"}");
    }
    engine_.stop();
    if (send_stop) {
        nlohmann::json request = {
            {"jsonrpc", "2.0"},
            {"id", request_id},
            {"method", "client_puck_voice_stop"},
            {"params", nlohmann::json::object()},
        };
        send_json(request);
    }
    log::error("voice: %s", error.c_str());
}

CallView CallController::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}

PuckVisual CallController::visual() const {
    const CallView current = view();
    PuckVisual visual;
    visual.speaking = current.speaking;
    visual.listening = current.phase == CallPhase::Active && !current.muted;
    visual.thinking = current.thinking;
    visual.not_listening = current.muted;
    visual.mouth = current.mouth;
    return visual;
}

bool CallController::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_.in_call();
}

bool CallController::ended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_;
}

bool CallController::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

std::string CallController::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

}  // namespace pk
