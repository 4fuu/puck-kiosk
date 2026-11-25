#pragma once

#include "core/types.h"
#include "platform/http.h"
#include "voice/engine.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace pk {

struct CallOptions {
    int answer_timeout_seconds = 15;
    int connect_timeout_seconds = 15;
    int offer_timeout_seconds = 10;
    int model_ready_timeout_seconds = 15;
    int close_timeout_seconds = 5;
    int mute_after_seconds = -1;
    int end_after_seconds = -1;
};

// Times the call's two local windows from the Web voice client: output audio
// keeps the speaking pose for 500 ms, and userSpeaking keeps the listening pose
// for one second after it ends. Levels are already on the Web scale (output RMS
// x4, input RMS x6).
struct CallSignal {
    static constexpr float kSpeakingThreshold = 0.03f;
    static constexpr std::chrono::milliseconds kSpeakingHold{500};
    static constexpr std::chrono::milliseconds kUserSpeakingHold{1000};

    void update_output(float level, std::chrono::steady_clock::time_point now);
    void set_user_speaking(bool speaking, std::chrono::steady_clock::time_point now);
    void barge_in(std::chrono::steady_clock::time_point now);
    bool speaking(std::chrono::steady_clock::time_point now) const;
    bool user_active(std::chrono::steady_clock::time_point now) const;
    bool user_talking() const { return user_speaking_; }

private:
    bool output_above_ = false;
    std::chrono::steady_clock::time_point output_last_above_{};
    bool user_speaking_ = false;
    std::chrono::steady_clock::time_point user_hold_until_{};
};

// Drives the actor JSON-RPC side of a voice call: offer, answer, mute, stop,
// voice-state notifications, and the data channel's session.closed event. The
// engine owns media; this class owns protocol and UI state.
class CallController {
public:
    CallController(WebSocket& actor, voice::VoiceMedia& engine, CallOptions options = {});
    ~CallController();

    bool start(std::string* error);
    void rebind_actor(WebSocket& actor);
    bool reattach();
    void on_actor_message(const std::string& text);
    void on_data_message(const std::string& text);
    void tick(std::chrono::steady_clock::time_point now);

    void mute(bool muted);
    void end(const std::string& reason);
    // The app failed an external recovery step (reattach rejected/timeout)
    // while the media session may still be alive. Runs the same close sequence
    // as an internal failure so the peer is released only after confirmation.
    void fail_recovery(const std::string& error);

    CallView view() const;
    PuckVisual visual() const;
    bool active() const;
    // True once the end/failure close sequence released the peer.
    bool ended() const;
    bool failed() const;
    std::string last_error() const;
    // The actor session id from the start answer; used to reattach after a
    // signaling-only reconnect without starting a second session.
    std::string session_id() const;

private:
    void handle_actor_object(const nlohmann::json& message);
    void handle_voice_state(const nlohmann::json& params);
    void handle_transcript(const nlohmann::json& params);
    void handle_answer(const nlohmann::json& payload);
    bool send_json(const nlohmann::json& value);
    bool send_stop_request();
    void begin_shutdown(bool failure, const std::string& reason);
    void finish_shutdown(bool confirmed);
    void apply_input_gate();
    void fail(const std::string& error);

    WebSocket* actor_;
    voice::VoiceMedia& engine_;
    CallOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable close_cv_;
    CallView view_;
    CallSignal signal_;
    std::string last_error_;
    std::string transcript_;
    std::string remote_status_;
    std::string remote_close_reason_;
    std::string session_id_;
    int answer_request_id_ = 0;
    int stop_request_id_ = 0;
    int attach_request_id_ = 0;

    bool start_called_ = false;
    bool start_request_sent_ = false;
    bool answer_received_ = false;
    bool ever_connected_ = false;
    bool ended_ = false;
    bool failed_ = false;
    bool stopping_ = false;
    bool stopped_ = false;
    bool muted_ = false;
    bool dictation_paused_ = false;
    bool input_silenced_ = false;
    bool model_ready_ = false;
    bool chime_eligible_ = false;
    bool chime_done_ = false;
    bool thinking_ = false;
    bool remote_closed_ = false;
    bool remote_failed_ = false;
    bool session_close_sent_ = false;
    bool pending_start_ = false;
    bool stop_pending_ = false;
    bool stop_ok_ = false;
    bool model_ready_deadline_set_ = false;
    bool scheduled_mute_ = false;
    bool scheduled_end_ = false;
    std::chrono::steady_clock::time_point call_started_;
    std::chrono::steady_clock::time_point offer_deadline_;
    std::chrono::steady_clock::time_point answer_deadline_;
    std::chrono::steady_clock::time_point connect_deadline_;
    std::chrono::steady_clock::time_point model_ready_deadline_;
    std::chrono::steady_clock::time_point close_deadline_;

    std::thread offer_thread_;
    bool offer_done_ = false;
    bool offer_ok_ = false;
    std::string offer_sdp_;
    std::string offer_error_;
};

}  // namespace pk
