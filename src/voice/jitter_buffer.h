#pragma once

#include "voice/rtp.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace pk::voice {

// Receive-side jitter buffer for Opus-over-RTP. libdatachannel deliberately
// stops at the RTP packet boundary; this class supplies the reordering,
// adaptive playout delay, loss concealment signal, and NACK requests that the
// browser's WebRTC stack normally provides.
//
// All public methods are safe to call from the network callback thread, the
// playback thread, and the UI thread. Playout is driven by calling pop() once
// per frame; take_nack_requests() is called after each pop.
class ReceiveJitterBuffer {
public:
    struct Config {
        int clock_rate = 48000;
        int frame_samples = 960;
        int initial_delay_frames = 3;
        int min_delay_frames = 1;
        int max_delay_frames = 12;
        int max_buffered_packets = 64;
        int nack_window_frames = 16;
        int nack_max_requests = 2;
        int nack_retry_rounds = 2;
        int decrease_after_frames = 250;
    };

    enum class PopKind {
        Empty,
        Frame,
        Concealed,
    };

    struct PopResult {
        PopKind kind = PopKind::Empty;
        std::vector<std::uint8_t> payload;
        std::uint32_t timestamp = 0;
    };

    struct Stats {
        std::uint64_t received = 0;
        std::uint64_t played = 0;
        std::uint64_t concealed = 0;
        std::uint64_t reordered = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t late = 0;
        std::uint64_t dropped = 0;
        std::uint64_t nack_requests = 0;
        std::uint64_t target_increases = 0;
        std::uint64_t target_decreases = 0;
    };

    ReceiveJitterBuffer();
    explicit ReceiveJitterBuffer(Config config);

    void reset();
    bool push(const RtpPacket& packet);
    PopResult pop();
    std::vector<std::uint16_t> take_nack_requests();

    int target_delay_frames() const;
    int buffered_frames() const;
    bool playout_started() const;
    Stats stats() const;

private:
    struct NackState {
        int requests = 0;
        std::uint64_t last_round = 0;
    };

    void increase_target();

    Config config_;
    mutable std::mutex mutex_;
    std::map<std::int64_t, RtpPacket> packets_;
    std::map<std::int64_t, NackState> nack_;
    std::int64_t next_ext_ = 0;
    std::int64_t highest_ext_ = 0;
    std::uint32_t last_timestamp_ = 0;
    bool have_packets_ = false;
    bool playout_started_ = false;
    int target_delay_ = 0;
    int frames_since_loss_ = 0;
    std::uint64_t nack_round_ = 0;
    Stats stats_;
};

}  // namespace pk::voice
