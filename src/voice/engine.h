#pragma once

#include "platform/audio.h"
#include "voice/aec.h"
#include "voice/peer_connection.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pk::voice {

class EchoCanceller;
class OpusDecoder;
class OpusEncoder;

struct VoiceEngineOptions {
    int sample_rate = 48000;
    int channels = 1;
    int frame_ms = 20;
    int opus_bitrate = 32000;
    bool enable_aec = true;
    int jitter_initial_delay_frames = 3;
    int jitter_max_delay_frames = 12;

    // Loopback diagnostics only. 0 disables the impairment.
    int test_reorder_every = 0;
    int test_drop_every = 0;
};

struct VoiceEngineStats {
    std::uint64_t capture_frames = 0;
    std::uint64_t encoded_frames = 0;
    std::uint64_t sent_packets = 0;
    std::uint64_t received_packets = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t concealed_frames = 0;
    std::uint64_t late_packets = 0;
    std::uint64_t reordered_packets = 0;
    std::uint64_t duplicate_packets = 0;
    std::uint64_t dropped_packets = 0;
    std::uint64_t nack_requests = 0;
    std::uint64_t retransmitted_packets = 0;
    std::uint64_t ready_chimes = 0;
    int jitter_target_frames = 0;
    int jitter_buffered_frames = 0;
    float input_level = 0.0f;
    float output_level = 0.0f;
    float output_mouth = 0.0f;
    bool connected = false;
    PeerState peer_state = PeerState::New;
};

// The media operations the call state machine needs. VoiceEngine implements
// this; tests can supply a deterministic media without ICE or a sound device.
class VoiceMedia {
public:
    virtual ~VoiceMedia() = default;

    virtual bool create_offer(std::string* sdp, std::string* error) = 0;
    virtual bool accept_answer(const std::string& answer_sdp, std::string* error) = 0;
    virtual bool start_audio(std::string* error) = 0;
    virtual void stop_audio() = 0;
    virtual void stop() = 0;
    virtual void set_input_silenced(bool silenced) = 0;
    virtual void play_ready_chime() = 0;
    virtual bool send_data(const std::string& text) = 0;
    virtual bool connected() const = 0;
    virtual PeerState peer_state() const = 0;
    virtual VoiceEngineStats stats() const = 0;
};

// Owns the capture/playback threads, Opus codecs, AEC, jitter buffer, and one
// VoicePeer. The app drives the call state machine; this class moves audio.
class VoiceEngine final : public VoiceMedia {
public:
    VoiceEngine(AudioIO& audio, VoiceEngineOptions options = {});
    ~VoiceEngine();
    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;

    bool create_offer(std::string* sdp, std::string* error) override;
    bool accept_offer(const std::string& offer_sdp, std::string* answer_sdp, std::string* error);
    bool accept_answer(const std::string& answer_sdp, std::string* error) override;

    // Opens capture and playback before the offer exists; this is the pre-call
    // device check and keeps the first sentence flowing across the ready cue.
    bool start_audio(std::string* error) override;
    void stop_audio() override;
    void stop() override;

    // Manual mute, dictation pause, and leaving are combined by the app.
    void set_input_silenced(bool silenced) override;
    // One local two-tone cue for the peer-and-model-ready transition.
    void play_ready_chime() override;

    bool send_data(const std::string& text) override;
    void set_data_callback(std::function<void(const std::string&)> callback);
    void set_peer_state_callback(std::function<void(PeerState)> callback);

    bool connected() const override;
    PeerState peer_state() const override;
    VoiceEngineStats stats() const override;
    std::string last_error() const;
    const char* audio_name() const;
    // The EchoCanceller that is actually running this call. `initialized` is
    // false when AEC is off, unavailable, or the audio threads are stopped.
    voice::AecStatus aec_status() const;

private:
    struct Shared;

    void capture_loop();
    void playback_loop();
    bool create_peer(std::string* error);
    std::shared_ptr<VoicePeer> peer_snapshot() const;
    void set_error(const std::string& error);

    AudioIO& audio_;
    VoiceEngineOptions options_;
    int frame_samples_ = 960;
    std::shared_ptr<Shared> shared_;
    mutable std::mutex peer_mutex_;
    std::shared_ptr<VoicePeer> peer_;
    std::atomic<bool> stop_requested_{false};
    std::unique_ptr<OpusEncoder> encoder_;
    std::unique_ptr<OpusDecoder> decoder_;
    std::unique_ptr<EchoCanceller> aec_;
    mutable std::mutex aec_mutex_;
    voice::AecStatus aec_status_;
    std::thread capture_thread_;
    std::thread playback_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> audio_started_{false};
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::uint32_t rtp_timestamp_ = 0;
};

}  // namespace pk::voice
