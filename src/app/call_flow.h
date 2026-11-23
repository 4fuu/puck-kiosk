#pragma once

#include "core/types.h"
#include "platform/http.h"
#include "voice/engine.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>

namespace pk {

struct CallOptions {
    int answer_timeout_seconds = 15;
    int connect_timeout_seconds = 15;
    int mute_after_seconds = -1;
    int end_after_seconds = -1;
};

// Drives the actor JSON-RPC side of a voice call: offer, answer, mute, stop,
// voice-state notifications, and the data channel's session.closed event. The
// engine owns media; this class owns protocol and UI state.
class CallController {
public:
    CallController(WebSocket& actor, voice::VoiceEngine& engine, CallOptions options = {});
    ~CallController();

    bool start(std::string* error);
    void rebind_actor(WebSocket& actor);
    bool reattach();
    void on_actor_message(const std::string& text);
    void on_data_message(const std::string& text);
    void tick(std::chrono::steady_clock::time_point now);

    void mute(bool muted);
    void end(const std::string& reason);

    CallView view() const;
    PuckVisual visual() const;
    bool active() const;
    bool ended() const;
    bool failed() const;
    std::string last_error() const;

private:
    void handle_actor_object(const nlohmann::json& message);
    void handle_voice_state(const nlohmann::json& params);
    void handle_transcript(const nlohmann::json& params);
    void handle_answer(const nlohmann::json& payload);
    bool send_json(const nlohmann::json& value);
    void fail(const std::string& error);

    WebSocket* actor_;
    voice::VoiceEngine& engine_;
    CallOptions options_;

    mutable std::mutex mutex_;
    CallView view_;
    std::string last_error_;
    std::string transcript_;
    std::string remote_status_;
    std::string remote_close_reason_;
    std::string session_id_;
    int request_id_ = 0;
    int answer_request_id_ = 0;
    int stop_request_id_ = 0;
    int attach_request_id_ = 0;
    bool start_sent_ = false;
    bool answer_received_ = false;
    bool stop_sent_ = false;
    bool ended_ = false;
    bool failed_ = false;
    bool muted_ = false;
    bool scheduled_mute_ = false;
    bool scheduled_end_ = false;
    bool puck_speaking_ = false;
    bool user_speaking_ = false;
    bool thinking_ = false;
    bool remote_closed_ = false;
    bool remote_failed_ = false;
    std::chrono::steady_clock::time_point call_started_;
    std::chrono::steady_clock::time_point answer_deadline_;
    std::chrono::steady_clock::time_point connect_deadline_;
};

}  // namespace pk
