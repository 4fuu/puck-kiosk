#include "app/call_flow.h"
#include "platform/audio.h"
#include "voice/engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

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

class FakeSocket final : public pk::WebSocket {
public:
    bool send_text(const std::string& text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back(text);
        return open_;
    }

    bool poll(std::string&, std::string*) override { return false; }

    bool is_open() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

    std::string close_reason() const override { return {}; }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }

    std::vector<std::string> sent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> sent_;
    bool open_ = true;
};

class FakeMedia final : public pk::voice::VoiceMedia {
public:
    bool fail_audio = false;
    std::atomic<bool> channel_open{false};
    std::atomic<float> input{0.5f};
    std::atomic<bool> connected_{false};
    std::atomic<pk::voice::PeerState> peer_state_{pk::voice::PeerState::New};
    std::atomic<int> start_audio_calls{0};
    std::atomic<int> stop_audio_calls{0};
    std::atomic<int> stop_calls{0};
    std::atomic<int> chimes{0};
    std::atomic<int> answers{0};
    std::atomic<std::uint64_t> sent_packets{0};
    std::atomic<std::uint64_t> capture_frames{0};
    std::atomic<float> output{0.0f};
    std::atomic<float> mouth{0.0f};

    bool create_offer(std::string* sdp, std::string* error) override {
        if (start_audio_calls.load() == 0) {
            if (error) {
                *error = "offer before the device check";
            }
            return false;
        }
        *sdp = "fake-offer";
        return true;
    }

    bool accept_answer(const std::string& answer_sdp, std::string* error) override {
        ++answers;
        if (answer_sdp != "fake-answer") {
            if (error) {
                *error = "unexpected answer";
            }
            return false;
        }
        connected_ = true;
        peer_state_ = pk::voice::PeerState::Connected;
        return true;
    }

    bool start_audio(std::string* error) override {
        if (fail_audio) {
            if (error) {
                *error = "microphone unavailable: no microphone";
            }
            return false;
        }
        ++start_audio_calls;
        return true;
    }

    void stop_audio() override {
        ++stop_audio_calls;
        input = 0.0f;
        output = 0.0f;
        mouth = 0.0f;
    }

    void stop() override {
        ++stop_calls;
        connected_ = false;
        peer_state_ = pk::voice::PeerState::Closed;
        input = 0.0f;
        output = 0.0f;
        mouth = 0.0f;
    }

    void set_input_silenced(bool silenced) override {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            gate_calls_.push_back(silenced);
        }
        silenced_ = silenced;
        if (silenced) {
            input = 0.0f;
        }
    }

    std::vector<bool> gate_calls() const {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        return gate_calls_;
    }

    void clear_gate_calls() {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        gate_calls_.clear();
    }

    void play_ready_chime() override { ++chimes; }

    bool send_data(const std::string& text) override {
        if (!channel_open.load()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(data_mutex_);
        data_.push_back(text);
        return true;
    }

    bool connected() const override { return connected_.load(); }
    pk::voice::PeerState peer_state() const override { return peer_state_.load(); }

    pk::voice::VoiceEngineStats stats() const override {
        pk::voice::VoiceEngineStats stats;
        stats.connected = connected_.load();
        stats.peer_state = peer_state_.load();
        stats.input_level = silenced_.load() ? 0.0f : input.load();
        stats.output_level = output.load();
        stats.output_mouth = mouth.load();
        stats.sent_packets = sent_packets.load();
        stats.capture_frames = capture_frames.load();
        stats.ready_chimes = static_cast<std::uint64_t>(chimes.load());
        stats.jitter_target_frames = 3;
        return stats;
    }

    std::vector<std::string> data_messages() const {
        std::lock_guard<std::mutex> lock(data_mutex_);
        return data_;
    }

private:
    std::atomic<bool> silenced_{false};
    mutable std::mutex gate_mutex_;
    std::vector<bool> gate_calls_;
    mutable std::mutex data_mutex_;
    std::vector<std::string> data_;
};

struct ActiveCall {
    FakeSocket& socket;
    FakeMedia media;
    std::unique_ptr<pk::CallController> call;
    pk::CallOptions options;
    int start_id = 0;

    ActiveCall(FakeSocket& socket_ref, pk::CallOptions call_options = {})
        : socket(socket_ref), options(call_options) {
        call = std::make_unique<pk::CallController>(socket, media, options);
    }
};

nlohmann::json find_request(const FakeSocket& socket, const std::string& method,
                            int skip = 0) {
    for (const std::string& text : socket.sent()) {
        const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
        if (!message.is_object() || message.value("method", "") != method) {
            continue;
        }
        if (skip > 0) {
            --skip;
            continue;
        }
        return message;
    }
    return nlohmann::json();
}

nlohmann::json find_last_request(const FakeSocket& socket, const std::string& method) {
    nlohmann::json result;
    for (const std::string& text : socket.sent()) {
        const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
        if (message.is_object() && message.value("method", "") == method) {
            result = message;
        }
    }
    return result;
}

int count_method(const FakeSocket& socket, const std::string& method) {
    int count = 0;
    for (const std::string& text : socket.sent()) {
        const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
        if (message.is_object() && message.value("method", "") == method) {
            ++count;
        }
    }
    return count;
}

template <class Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void feed_state(ActiveCall& active, const nlohmann::json& params) {
    const nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"method", "puck_voice_state"},
        {"params", params},
    };
    active.call->on_actor_message(message.dump());
}

nlohmann::json complete_state(const std::string& status, bool user, bool puck,
                              int pause = -1) {
    nlohmann::json params = {
        {"status", status},
        {"userSpeaking", user},
        {"puckSpeaking", puck},
    };
    if (pause == 0) {
        params["inputPausedForDictation"] = false;
    } else if (pause == 1) {
        params["inputPausedForDictation"] = true;
    }
    return params;
}

bool drive_start_request(ActiveCall& active, std::string* error) {
    const int requests_before = count_method(active.socket, "client_puck_voice_start");
    if (active.media.start_audio_calls.load() != 1) {
        *error = "the microphone was not opened before the session request";
        return false;
    }
    if (count_method(active.socket, "client_puck_voice_start") != requests_before) {
        *error = "the start request was already sent";
        return false;
    }

    if (!wait_for(
            [&] {
                active.call->tick(Clock::now());
                return count_method(active.socket, "client_puck_voice_start") >
                       requests_before;
            },
            2s)) {
        *error = "no start request";
        return false;
    }
    const nlohmann::json request =
        find_last_request(active.socket, "client_puck_voice_start");
    if (!request["id"].is_number_integer() ||
        !request["params"]["transport"]["sdp"].is_string()) {
        *error = "start request is malformed";
        return false;
    }
    active.start_id = request["id"].get<int>();
    return true;
}

bool start_and_request(ActiveCall& active, std::string* error) {
    if (!active.call->start(error)) {
        return false;
    }
    return drive_start_request(active, error);
}

void answer_call(ActiveCall& active) {
    const nlohmann::json response = {
        {"jsonrpc", "2.0"},
        {"id", active.start_id},
        {"result",
         {{"transport", {{"sdp", "fake-answer"}}},
          {"sessionID", "fake-session"}}},
    };
    active.call->on_actor_message(response.dump());
    active.call->tick(Clock::now());
}

void ack_last_stop(ActiveCall& active) {
    const nlohmann::json stop =
        find_last_request(active.socket, "client_puck_voice_stop");
    if (stop.is_null()) {
        return;
    }
    const nlohmann::json response = {
        {"jsonrpc", "2.0"},
        {"id", stop["id"]},
        {"result", nlohmann::json::object()},
    };
    active.call->on_actor_message(response.dump());
}

bool make_ready(ActiveCall& active, std::string* error) {
    feed_state(active,
               {{"status", "listening"}, {"userSpeaking", false}, {"puckSpeaking", false}});
    active.call->tick(Clock::now());
    if (!active.call->view().ready) {
        *error = "call never became ready";
        return false;
    }
    return true;
}

void test_signal_windows() {
    pk::CallSignal signal;
    const Clock::time_point t0 = Clock::now();

    signal.update_output(0.029f, t0);
    CHECK(!signal.speaking(t0));
    signal.update_output(0.03f, t0);
    CHECK(signal.speaking(t0));
    CHECK(signal.speaking(t0 + 499ms));
    CHECK(signal.speaking(t0 + 500ms));
    CHECK(!signal.speaking(t0 + 501ms));

    signal.update_output(0.4f, t0 + 1000ms);
    CHECK(signal.speaking(t0 + 1400ms));
    CHECK(signal.speaking(t0 + 1499ms));
    CHECK(!signal.speaking(t0 + 1501ms));

    signal.update_output(0.4f, t0 + 2000ms);
    signal.barge_in(t0 + 2100ms);
    CHECK(!signal.speaking(t0 + 2100ms));

    signal.set_user_speaking(true, t0 + 3000ms);
    CHECK(signal.user_active(t0 + 30s));
    signal.set_user_speaking(false, t0 + 4000ms);
    CHECK(signal.user_active(t0 + 4999ms));
    CHECK(!signal.user_active(t0 + 5000ms));

    signal.set_user_speaking(true, t0 + 6000ms);
    signal.set_user_speaking(false, t0 + 6200ms);
    CHECK(signal.user_active(t0 + 7199ms));
    CHECK(!signal.user_active(t0 + 7200ms));
}

void test_device_check_blocks_session() {
    FakeSocket socket;
    ActiveCall active(socket);
    active.media.fail_audio = true;

    std::string error;
    CHECK(!active.call->start(&error));
    CHECK_MSG(error.find("microphone unavailable") != std::string::npos, error);
    CHECK(socket.sent().empty());
    CHECK(active.call->failed());
    CHECK(active.call->view().phase == pk::CallPhase::Failed);
    CHECK(active.call->view().closing == false);
    CHECK(active.media.start_audio_calls.load() == 0);
    CHECK_MSG(active.call->last_error().find("microphone unavailable") !=
                  std::string::npos,
              active.call->last_error());
}

void test_ready_order_and_first_audio() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }

    CHECK_MSG(active.media.start_audio_calls.load() == 1,
              "the device is opened once before the session request");
    CHECK_MSG(active.media.stop_audio_calls.load() == 0,
              "audio stays open while ICE and the answer are pending");
    active.media.sent_packets = 42;
    active.media.capture_frames = 10;

    active.call->tick(Clock::now());
    CHECK(!active.call->view().ready);
    CHECK(active.call->view().phase == pk::CallPhase::Starting);
    CHECK(active.call->view().input_level == 0.0f);
    CHECK_MSG(active.media.chimes.load() == 0, "no cue before the model is ready");

    answer_call(active);
    CHECK(active.media.answers.load() == 1);
    CHECK_MSG(active.media.start_audio_calls.load() == 1,
              "applying the answer does not reopen the device");
    CHECK(active.call->view().phase == pk::CallPhase::Connecting);
    CHECK(active.call->view().peer_connected);
    CHECK(active.call->view().caption == "Waiting for Puck");
    CHECK(!active.call->view().ready);
    CHECK(active.call->view().input_level == 0.0f);
    CHECK_MSG(active.media.stop_audio_calls.load() == 0,
              "applying the answer does not restart audio");

    feed_state(active,
               {{"status", "listening"}, {"userSpeaking", false}, {"puckSpeaking", false}});
    active.call->tick(Clock::now());
    CHECK(active.call->view().ready);
    CHECK(active.call->view().peer_connected);
    CHECK(active.call->view().phase == pk::CallPhase::Active);
    CHECK(active.call->view().input_level > 0.0f);
    CHECK(active.media.chimes.load() == 1);

    feed_state(active,
               {{"status", "listening"}, {"userSpeaking", false}, {"puckSpeaking", false}});
    active.call->tick(Clock::now());
    CHECK_MSG(active.media.chimes.load() == 1, "the ready cue is not repeated");

    active.media.capture_frames = 20;
    active.call->tick(Clock::now());
    CHECK_MSG(active.media.capture_frames.load() == 20,
              "capture continues across the ready transition");
    CHECK(active.media.stop_audio_calls.load() == 0);
    CHECK(active.media.sent_packets.load() == 42);
}

void test_mute_and_dictation_gate() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));

    active.media.input = 0.5f;
    active.call->tick(Clock::now());
    CHECK(!active.call->view().input_silenced);
    CHECK(active.call->view().input_level > 0.0f);

    active.call->mute(true);
    CHECK(active.call->view().muted);
    CHECK(active.call->view().input_silenced);
    CHECK(active.call->view().input_level == 0.0f);
    CHECK(active.media.stats().input_level == 0.0f);

    nlohmann::json with_muted_false = complete_state("listening", false, false, 0);
    with_muted_false["muted"] = false;
    feed_state(active, with_muted_false);
    active.call->tick(Clock::now());
    CHECK_MSG(active.call->view().muted, "actor muted=false cannot clear local mute");
    CHECK(active.call->view().input_silenced);
    CHECK(active.call->view().input_level == 0.0f);

    active.call->mute(false);
    CHECK(!active.call->view().muted);
    CHECK(!active.call->view().input_silenced);

    active.media.clear_gate_calls();
    nlohmann::json pause_on = complete_state("listening", false, false, 1);
    pause_on["muted"] = false;
    feed_state(active, pause_on);
    active.call->tick(Clock::now());
    CHECK(active.call->view().dictation_paused);
    CHECK(active.call->view().input_silenced);
    CHECK(active.call->view().input_level == 0.0f);
    const std::vector<bool> on_calls = active.media.gate_calls();
    CHECK_MSG(on_calls.size() == 1 && on_calls.front(),
              "a dictation packet closes the gate once, without a transient open");

    active.call->mute(true);
    CHECK(active.call->view().muted);
    CHECK(active.call->view().input_silenced);
    active.call->mute(false);
    CHECK(!active.call->view().muted);
    CHECK(active.call->view().dictation_paused);
    CHECK_MSG(active.call->view().input_silenced,
              "unmute cannot open a dictation pause");

    feed_state(active, complete_state("listening", false, false));
    active.call->tick(Clock::now());
    CHECK(!active.call->view().dictation_paused);
    CHECK(!active.call->view().input_silenced);
    active.media.input = 0.5f;
    active.call->tick(Clock::now());
    CHECK(active.call->view().input_level > 0.0f);

    feed_state(active, complete_state("listening", false, false, 1));
    CHECK(active.call->view().dictation_paused);
    active.media.clear_gate_calls();
    feed_state(active, {{"status", "listening"}});
    active.call->tick(Clock::now());
    CHECK(active.call->view().dictation_paused);
    CHECK(active.call->view().input_silenced);
    CHECK_MSG(active.media.gate_calls().empty(),
              "an incomplete legacy snapshot does not open the gate");

    nlohmann::json malformed = complete_state("listening", false, false);
    malformed["inputPausedForDictation"] = "yes";
    feed_state(active, malformed);
    active.call->tick(Clock::now());
    CHECK(active.call->view().dictation_paused);
    CHECK(active.call->view().input_silenced);
    CHECK_MSG(active.media.gate_calls().empty(),
              "a malformed pause value is not an explicit release");

    feed_state(active, complete_state("listening", false, false, 0));
    active.call->tick(Clock::now());
    CHECK(!active.call->view().dictation_paused);
    CHECK(!active.call->view().input_silenced);

    feed_state(active, complete_state("listening", false, false, 1));
    CHECK(active.call->view().dictation_paused);
    active.call->mute(true);
    CHECK(active.call->view().muted);
    CHECK(active.call->view().input_silenced);
    feed_state(active, complete_state("listening", false, false, 0));
    CHECK(!active.call->view().dictation_paused);
    CHECK_MSG(active.call->view().muted && active.call->view().input_silenced,
              "dictation release cannot open a local mute");
    active.call->mute(false);
    CHECK(!active.call->view().muted);
    CHECK(!active.call->view().input_silenced);

    CHECK_MSG(count_method(socket, "client_puck_voice_user_state") == 6,
              "only manual mute changes are announced to the actor");
}

void test_current_terminal_does_not_release_dictation() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));
    feed_state(active, complete_state("listening", false, false, 1));
    active.call->tick(Clock::now());
    CHECK(active.call->view().dictation_paused);
    CHECK(active.call->view().input_silenced);
    active.media.clear_gate_calls();

    feed_state(active, complete_state("ended", false, false));
    CHECK_MSG(active.media.gate_calls().empty(),
              "a terminal snapshot does not reopen the dictation gate");
    CHECK(active.call->view().dictation_paused);
    CHECK(active.call->view().input_silenced);

    active.call->tick(Clock::now());
    CHECK(active.call->view().phase == pk::CallPhase::Ended);
    CHECK(active.call->view().closing);
    CHECK(active.call->view().input_silenced);
    ack_last_stop(active);
    active.call->tick(Clock::now());
    CHECK(active.call->ended());
    CHECK_MSG(active.call->view().close_confirmed,
              "the current call's terminal snapshot still ends the call");
}

void test_output_drives_speaking() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));

    active.media.output = 0.0f;
    active.media.mouth = 0.0f;
    feed_state(active, complete_state("listening", false, true));
    active.call->tick(Clock::now());
    CHECK_MSG(!active.call->view().speaking,
              "actor puckSpeaking alone does not drive the WebRTC stage");
    CHECK(active.call->view().mouth == 0.0f);

    const Clock::time_point t0 = Clock::now();
    active.media.output = 0.2f;
    active.media.mouth = 0.3f;
    active.call->tick(t0);
    CHECK(active.call->view().speaking);
    CHECK(active.call->view().mouth == 0.3f);

    active.media.output = 0.0f;
    active.media.mouth = 0.0f;
    feed_state(active, complete_state("listening", false, true));
    active.call->tick(t0 + 300ms);
    CHECK_MSG(active.call->view().speaking, "short output gaps keep speaking");
    active.call->tick(t0 + 700ms);
    CHECK_MSG(!active.call->view().speaking,
              "700 ms of silence ends the 500 ms speaking window");
    CHECK(active.call->view().mouth == 0.0f);

    active.media.output = 0.2f;
    active.media.mouth = 0.3f;
    active.call->tick(t0 + 800ms);
    CHECK(active.call->view().speaking);
    feed_state(active, complete_state("listening", true, true));
    active.call->tick(t0 + 850ms);
    CHECK_MSG(!active.call->view().speaking, "barge-in clears speaking immediately");
    CHECK(active.call->view().user_speaking);
    CHECK(active.call->view().mouth == 0.0f);
    CHECK(active.call->view().output_level == 0.0f);
    CHECK(active.call->visual().listening);
    CHECK_MSG(active.media.connected_.load(), "barge-in does not stop media");
    CHECK(active.media.stop_audio_calls.load() == 0);
}

void test_model_ready_is_bounded() {
    FakeSocket socket;
    pk::CallOptions options;
    options.model_ready_timeout_seconds = 1;
    ActiveCall active(socket, options);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(active.call->view().phase == pk::CallPhase::Connecting);
    CHECK(active.call->view().peer_connected);
    CHECK(!active.call->view().ready);

    const Clock::time_point t0 = Clock::now();
    active.call->tick(t0);
    CHECK(active.call->view().phase == pk::CallPhase::Connecting);
    CHECK(active.call->view().peer_connected);
    CHECK_MSG(!active.call->view().ready, "the model is not ready yet");

    active.call->tick(t0 + 2s);
    CHECK_MSG(active.call->view().phase == pk::CallPhase::Failed,
              "a peer that never gets a model-ready state is bounded");
    CHECK_MSG(active.call->view().detail.find("model") != std::string::npos,
              active.call->view().detail);
    CHECK(!active.call->view().peer_connected);

    ack_last_stop(active);
    active.call->tick(Clock::now());
    CHECK(active.call->failed());
}

void test_stale_voice_state_before_start_is_ignored() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(active.call->start(&error), error);
    if (!error.empty()) {
        return;
    }
    CHECK(count_method(socket, "client_puck_voice_start") == 0);
    feed_state(active, complete_state("listening", false, false));
    feed_state(active, complete_state("ended", false, false));

    CHECK_MSG(drive_start_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    active.call->tick(Clock::now());
    CHECK(!active.call->view().ready);
    CHECK(active.call->view().phase == pk::CallPhase::Connecting);
    CHECK(!active.call->ended());
    CHECK_MSG(active.media.chimes.load() == 0,
              "an observer's old listening state does not make the new call ready");

    feed_state(active, complete_state("listening", false, false));
    active.call->tick(Clock::now());
    CHECK(active.call->view().ready);
    CHECK(active.call->view().phase == pk::CallPhase::Active);
    CHECK(active.media.chimes.load() == 1);
    CHECK_MSG(!active.call->ended(),
              "the old ended state does not end the call after its own listening state");

    active.call->end("cleanup");
    ack_last_stop(active);
    active.call->tick(Clock::now());
    CHECK(active.call->ended());
    CHECK(active.call->view().close_confirmed);
}

void test_close_waits_for_confirmation() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));
    active.media.channel_open = true;

    active.call->end("test hangup");
    CHECK_MSG(!active.call->ended(), "the peer is not released before session.closed");
    CHECK(active.call->view().phase == pk::CallPhase::Ended);
    CHECK(active.call->view().closing);
    CHECK_MSG(active.call->view().in_call(),
              "the call screen stays until the peer is released");
    CHECK(active.call->active());
    CHECK(active.call->view().input_level == 0.0f);
    CHECK(active.call->view().output_level == 0.0f);
    CHECK(active.media.stop_audio_calls.load() == 1);
    CHECK(active.media.stop_calls.load() == 0);
    CHECK(active.media.data_messages().size() == 1);
    CHECK(active.media.data_messages().front().find("session.close") != std::string::npos);

    ack_last_stop(active);
    CHECK_MSG(!active.call->ended(),
              "the stop receipt alone does not release the peer");
    active.call->on_data_message("{\"type\":\"session.closed\",\"reason\":\"done\"}");
    active.call->tick(Clock::now());
    CHECK(active.call->ended());
    CHECK_MSG(active.call->view().close_confirmed, "stop and channel close confirmed");
    CHECK(!active.call->view().closing);
    CHECK(!active.call->view().in_call());
    CHECK(!active.call->active());
    CHECK(active.media.stop_calls.load() == 1);
    CHECK(active.media.peer_state() == pk::voice::PeerState::Closed);
    CHECK(count_method(socket, "client_puck_voice_stop") == 1);
    const nlohmann::json stop = find_request(socket, "client_puck_voice_stop");
    CHECK(stop["params"].is_object());
    CHECK_MSG(stop["params"].empty(), "the normal stop keeps the baseline empty params");
}

void test_close_timeout_releases_peer() {
    FakeSocket socket;
    pk::CallOptions options;
    options.close_timeout_seconds = 1;
    ActiveCall active(socket, options);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));
    active.media.channel_open = true;

    const Clock::time_point ended_at = Clock::now();
    active.call->end("timeout test");
    CHECK(!active.call->ended());
    CHECK(active.media.stop_audio_calls.load() == 1);
    CHECK(active.media.data_messages().size() == 1);
    ack_last_stop(active);
    CHECK(!active.call->ended());

    active.call->tick(ended_at + 500ms);
    CHECK_MSG(!active.call->ended(), "no release before the close timeout");
    CHECK_MSG(active.media.stop_calls.load() == 0, "peer stays alive while waiting");

    active.call->tick(ended_at + 1100ms);
    CHECK(active.call->ended());
    CHECK_MSG(!active.call->view().close_confirmed,
              "a session.closed timeout is not a confirmed close");
    CHECK(active.media.stop_calls.load() == 1);
    CHECK(active.media.peer_state() == pk::voice::PeerState::Closed);
    CHECK(count_method(socket, "client_puck_voice_stop") == 1);
}

void test_stop_error_is_not_confirmed() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));
    active.media.channel_open = true;

    active.call->end("stop error");
    const nlohmann::json stop = find_last_request(socket, "client_puck_voice_stop");
    CHECK(!stop.is_null());
    const nlohmann::json response = {
        {"jsonrpc", "2.0"},
        {"id", stop["id"]},
        {"error", {{"message", "not supported"}}},
    };
    active.call->on_actor_message(response.dump());
    active.call->on_data_message("{\"type\":\"session.closed\",\"reason\":\"done\"}");
    active.call->tick(Clock::now());
    CHECK(active.call->ended());
    CHECK_MSG(!active.call->view().close_confirmed,
              "a stop error receipt is not a confirmed close");
}

void test_stop_receipt_requires_rpc_response() {
    FakeSocket socket;
    {
        ActiveCall active(socket);
        std::string error;
        CHECK_MSG(start_and_request(active, &error), error);
        if (!error.empty()) {
            return;
        }
        answer_call(active);
        CHECK(make_ready(active, &error));
        active.call->end("receipt shape");
        const nlohmann::json stop = find_last_request(socket, "client_puck_voice_stop");
        CHECK(!stop.is_null());

        const nlohmann::json id_only = {{"id", stop["id"]}};
        active.call->on_actor_message(id_only.dump());
        active.call->tick(Clock::now());
        CHECK_MSG(!active.call->ended(), "an id-only frame is not a stop receipt");

        const nlohmann::json method_notification = {
            {"jsonrpc", "2.0"},
            {"id", stop["id"]},
            {"method", "puck_voice_state"},
            {"params", complete_state("listening", false, false)},
        };
        active.call->on_actor_message(method_notification.dump());
        active.call->tick(Clock::now());
        CHECK_MSG(!active.call->ended(), "a same-id method is not a stop receipt");
        CHECK(active.call->view().closing);

        const nlohmann::json void_result = {
            {"jsonrpc", "2.0"},
            {"id", stop["id"]},
        };
        active.call->on_actor_message(void_result.dump());
        active.call->tick(Clock::now());
        CHECK_MSG(active.call->ended(), "a JSON-RPC void response confirms the stop");
        CHECK(active.call->view().close_confirmed);
    }

    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    answer_call(active);
    CHECK(make_ready(active, &error));
    active.call->end("null result");
    const nlohmann::json stop = find_last_request(socket, "client_puck_voice_stop");
    CHECK(!stop.is_null());
    const nlohmann::json null_result = {
        {"jsonrpc", "2.0"},
        {"id", stop["id"]},
        {"result", nullptr},
    };
    active.call->on_actor_message(null_result.dump());
    active.call->tick(Clock::now());
    CHECK_MSG(active.call->ended(), "a JSON-RPC null result confirms the stop");
    CHECK(active.call->view().close_confirmed);
}

void test_pending_start_success_is_routed_before_release() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }
    CHECK(!active.media.channel_open.load());

    active.call->end("hangup before answer");
    CHECK(!active.call->ended());
    CHECK(active.call->view().closing);
    CHECK(active.call->view().in_call());
    CHECK(active.media.stop_audio_calls.load() == 1);
    CHECK(active.media.stop_calls.load() == 0);
    CHECK(count_method(socket, "client_puck_voice_stop") == 1);

    active.call->tick(Clock::now());
    CHECK_MSG(!active.call->ended(),
              "a pending start keeps the controller alive and routable");

    ack_last_stop(active);
    active.call->tick(Clock::now());
    CHECK_MSG(!active.call->ended(), "the pending start is still unresolved");

    const nlohmann::json late = {
        {"jsonrpc", "2.0"},
        {"id", active.start_id},
        {"result",
         {{"transport", {{"sdp", "fake-answer"}}},
          {"sessionID", "late-session"}}},
    };
    active.call->on_actor_message(late.dump());
    CHECK_MSG(active.media.answers.load() == 0,
              "the hung-up peer does not apply a late SDP");
    CHECK_MSG(count_method(socket, "client_puck_voice_stop") == 2,
              "a late start success is compensated with another stop");
    CHECK(active.call->view().phase == pk::CallPhase::Ended);
    CHECK(!active.call->ended());

    ack_last_stop(active);
    active.call->tick(Clock::now());
    CHECK(active.call->ended());
    CHECK_MSG(active.call->view().close_confirmed, "both stop receipts are confirmed");
    CHECK(active.media.stop_calls.load() == 1);
    CHECK(active.media.peer_state() == pk::voice::PeerState::Closed);
    for (const std::string& text : socket.sent()) {
        const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
        if (message.is_object() &&
            message.value("method", "") == "client_puck_voice_stop") {
            CHECK(message["params"].is_object());
            CHECK_MSG(message["params"].empty(), "stops use the known empty params");
        }
    }
    active.call.reset();
    CHECK_MSG(active.call == nullptr, "the app can release the controller at ended()");
}

void test_pending_start_rejection_is_routed_before_release() {
    FakeSocket socket;
    ActiveCall active(socket);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }

    active.call->end("hangup before answer");
    active.call->tick(Clock::now());
    CHECK(!active.call->ended());
    ack_last_stop(active);
    active.call->tick(Clock::now());
    CHECK(!active.call->ended());

    const nlohmann::json rejection = {
        {"jsonrpc", "2.0"},
        {"id", active.start_id},
        {"error", {{"message", "rejected"}}},
    };
    active.call->on_actor_message(rejection.dump());
    active.call->tick(Clock::now());
    CHECK_MSG(active.call->ended(), "a late rejection resolves the pending start");
    CHECK_MSG(active.call->view().close_confirmed,
              "a rejection plus stop receipt is a confirmed close");
    CHECK(active.media.answers.load() == 0);
    CHECK_MSG(count_method(socket, "client_puck_voice_stop") == 1,
              "a rejection needs no second stop");
    CHECK(active.media.stop_calls.load() == 1);
}

void test_pending_start_timeout_is_bounded() {
    FakeSocket socket;
    pk::CallOptions options;
    options.close_timeout_seconds = 1;
    ActiveCall active(socket, options);
    std::string error;
    CHECK_MSG(start_and_request(active, &error), error);
    if (!error.empty()) {
        return;
    }

    const Clock::time_point ended_at = Clock::now();
    active.call->end("hangup before answer");
    CHECK(!active.call->ended());
    ack_last_stop(active);
    active.call->tick(ended_at + 500ms);
    CHECK_MSG(!active.call->ended(), "a pending start is waited for inside the bound");
    active.call->tick(ended_at + 1100ms);
    CHECK_MSG(active.call->ended(), "the pending start wait is bounded");
    CHECK_MSG(!active.call->view().close_confirmed,
              "an unresolved start is not a confirmed close");
    CHECK(active.media.stop_calls.load() == 1);
    CHECK_MSG(count_method(socket, "client_puck_voice_stop") == 1,
              "no second stop can be sent without a late success");
}

void test_rapid_redial_ignores_old_answer() {
    FakeSocket socket;
    int old_id = 0;
    {
        ActiveCall first(socket);
        std::string error;
        CHECK_MSG(start_and_request(first, &error), error);
        if (!error.empty()) {
            return;
        }
        old_id = first.start_id;
        first.call->end("redial");
        CHECK_MSG(!first.call->ended(),
                  "the hanging-up call stays alive while its start is pending");
        ack_last_stop(first);
        first.call->tick(Clock::now());
        CHECK(!first.call->ended());

        const nlohmann::json late = {
            {"jsonrpc", "2.0"},
            {"id", old_id},
            {"result",
             {{"transport", {{"sdp", "fake-answer"}}},
              {"sessionID", "old-session"}}},
        };
        first.call->on_actor_message(late.dump());
        CHECK(first.media.answers.load() == 0);
        CHECK_MSG(count_method(socket, "client_puck_voice_stop") == 2,
                  "the old success is compensated before redial");
        ack_last_stop(first);
        first.call->tick(Clock::now());
        CHECK_MSG(first.call->ended(), "the old call releases after both receipts");
        CHECK(first.call->view().close_confirmed);
    }

    ActiveCall second(socket);
    std::string error;
    CHECK_MSG(start_and_request(second, &error), error);
    if (!error.empty()) {
        return;
    }
    CHECK_MSG(second.start_id != old_id, "redial uses a fresh request id");

    const int stops_before = count_method(socket, "client_puck_voice_stop");
    const nlohmann::json stale = {
        {"jsonrpc", "2.0"},
        {"id", old_id},
        {"result",
         {{"transport", {{"sdp", "fake-answer"}}},
          {"sessionID", "old-session"}}},
    };
    second.call->on_actor_message(stale.dump());

    const nlohmann::json stale_method = {
        {"jsonrpc", "2.0"},
        {"id", old_id},
        {"method", "client_puck_voice_start"},
        {"params", {{"transport", {{"sdp", "fake-answer"}}}}},
    };
    second.call->on_actor_message(stale_method.dump());

    const nlohmann::json uncorrelated = {
        {"jsonrpc", "2.0"},
        {"method", "client_puck_voice_start"},
        {"params", {{"transport", {{"sdp", "fake-answer"}}}}},
    };
    second.call->on_actor_message(uncorrelated.dump());

    const nlohmann::json same_id_method = {
        {"jsonrpc", "2.0"},
        {"id", second.start_id},
        {"method", "client_puck_voice_start"},
        {"params", {{"transport", {{"sdp", "fake-answer"}}}}},
    };
    second.call->on_actor_message(same_id_method.dump());

    second.call->tick(Clock::now());
    CHECK(second.call->view().phase == pk::CallPhase::Starting);
    CHECK(second.media.answers.load() == 0);
    CHECK(!second.media.connected_.load());
    CHECK_MSG(count_method(socket, "client_puck_voice_stop") == stops_before,
              "stale id or uncorrelated answers do not touch the new call");

    answer_call(second);
    CHECK_MSG(second.media.answers.load() == 1,
              "the new call still accepts its own id response");
    CHECK(second.call->view().phase == pk::CallPhase::Connecting);

    second.call->end("cleanup");
    ack_last_stop(second);
    second.call->tick(Clock::now());
    CHECK(second.call->ended());
    CHECK(second.call->view().close_confirmed);
}

class FailingAudio final : public pk::AudioIO {
public:
    const char* name() const override { return "failing"; }

    bool start_capture(int, int, std::string* error) override {
        if (error) {
            *error = "no microphone";
        }
        return false;
    }
    void stop_capture() override {}
    std::size_t read_capture(std::int16_t*, std::size_t) override { return 0; }

    bool start_playback(int, int, std::string*) override { return true; }
    void stop_playback() override {}
    std::size_t write_playback(const std::int16_t*, std::size_t) override { return 0; }
};

std::uint32_t read_u32(std::istream& in) {
    unsigned char bytes[4] = {};
    in.read(reinterpret_cast<char*>(bytes), 4);
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

int wav_peak(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;
    }
    char riff[4] = {};
    in.read(riff, 4);
    read_u32(in);
    char wave[4] = {};
    in.read(wave, 4);
    if (!in || std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
        return 0;
    }
    int peak = 0;
    while (in) {
        char id[4] = {};
        in.read(id, 4);
        if (!in) {
            break;
        }
        const std::uint32_t size = read_u32(in);
        const std::string chunk(id, 4);
        if (chunk == "data") {
            std::vector<char> bytes(size);
            in.read(bytes.data(), static_cast<std::streamsize>(size));
            for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
                const std::int16_t sample = static_cast<std::int16_t>(
                    static_cast<std::uint16_t>(
                        static_cast<unsigned char>(bytes[i]) |
                        (static_cast<unsigned char>(bytes[i + 1]) << 8)));
                peak = std::max(peak, std::abs(static_cast<int>(sample)));
            }
            break;
        }
        in.seekg(size, std::ios::cur);
        if (size % 2 != 0) {
            in.seekg(1, std::ios::cur);
        }
    }
    return peak;
}

void test_engine_device_error_and_levels() {
    FailingAudio failing;
    pk::voice::VoiceEngine failing_engine(failing);
    std::string error;
    CHECK(!failing_engine.start_audio(&error));
    CHECK_MSG(error.find("microphone unavailable") != std::string::npos, error);

    const std::string path = "/tmp/puck-call-flow-chime.wav";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    pk::FileAudioOptions audio_options;
    audio_options.playback_path = path;
    auto audio = pk::create_file_audio(audio_options, &error);
    CHECK(audio != nullptr);
    if (!audio) {
        return;
    }

    pk::voice::VoiceEngineOptions engine_options;
    engine_options.enable_aec = false;
    pk::voice::VoiceEngine engine(*audio, engine_options);
    CHECK_MSG(engine.start_audio(&error), error);
    CHECK(wait_for([&] { return engine.stats().input_level > 0.3f; }, 2s));
    CHECK_MSG(engine.stats().input_level > 0.3f,
              "input is on the Web scale (RMS x6), not raw RMS");

    engine.set_input_silenced(true);
    CHECK(wait_for([&] { return engine.stats().input_level == 0.0f; }, 1s));
    CHECK(engine.stats().input_level == 0.0f);
    engine.set_input_silenced(false);
    CHECK(wait_for([&] { return engine.stats().input_level > 0.3f; }, 2s));

    engine.play_ready_chime();
    CHECK(wait_for([&] { return engine.stats().ready_chimes == 1; }, 1s));
    std::this_thread::sleep_for(400ms);

    const std::uint64_t frames_before_stop = engine.stats().capture_frames;
    engine.stop_audio();
    CHECK(wait_for([&] { return engine.stats().input_level == 0.0f; }, 1s));
    const std::uint64_t frames_after_stop = engine.stats().capture_frames;
    std::this_thread::sleep_for(150ms);
    CHECK_MSG(engine.stats().capture_frames == frames_after_stop,
              "capture actually stops on hangup");
    CHECK(frames_after_stop >= frames_before_stop);
    CHECK_MSG(wav_peak(path) > 1000, "the ready cue reaches the playback sink");
    engine.stop();
    std::filesystem::remove(path, ignored);
}

}  // namespace

int main() {
    std::printf("[call-flow] signal windows\n");
    test_signal_windows();
    std::printf("[call-flow] pre-call device check\n");
    test_device_check_blocks_session();
    std::printf("[call-flow] ready order and first audio\n");
    test_ready_order_and_first_audio();
    std::printf("[call-flow] model readiness bound\n");
    test_model_ready_is_bounded();
    std::printf("[call-flow] stale voice state before start\n");
    test_stale_voice_state_before_start_is_ignored();
    std::printf("[call-flow] mute and dictation gate\n");
    test_mute_and_dictation_gate();
    std::printf("[call-flow] current terminal and dictation\n");
    test_current_terminal_does_not_release_dictation();
    std::printf("[call-flow] output-driven speaking\n");
    test_output_drives_speaking();
    std::printf("[call-flow] close confirmation\n");
    test_close_waits_for_confirmation();
    std::printf("[call-flow] close timeout\n");
    test_close_timeout_releases_peer();
    std::printf("[call-flow] stop error\n");
    test_stop_error_is_not_confirmed();
    std::printf("[call-flow] stop receipt shape\n");
    test_stop_receipt_requires_rpc_response();
    std::printf("[call-flow] pending start success\n");
    test_pending_start_success_is_routed_before_release();
    std::printf("[call-flow] pending start rejection\n");
    test_pending_start_rejection_is_routed_before_release();
    std::printf("[call-flow] pending start timeout\n");
    test_pending_start_timeout_is_bounded();
    std::printf("[call-flow] rapid redial\n");
    test_rapid_redial_ignores_old_answer();
    std::printf("[call-flow] engine audio levels and ready cue\n");
    test_engine_device_error_and_levels();

    if (g_failures == 0) {
        std::printf("[call-flow] PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("[call-flow] FAIL (%d/%d checks)\n", g_failures, g_checks);
    return 1;
}
