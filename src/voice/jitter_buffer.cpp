#include "voice/jitter_buffer.h"

#include <algorithm>

namespace pk::voice {
namespace {

std::int64_t extend_sequence(std::int64_t reference, std::uint16_t sequence) {
    const std::int16_t delta =
        static_cast<std::int16_t>(sequence - static_cast<std::uint16_t>(reference & 0xffff));
    return reference + delta;
}

}  // namespace

ReceiveJitterBuffer::ReceiveJitterBuffer() : ReceiveJitterBuffer(Config()) {}

ReceiveJitterBuffer::ReceiveJitterBuffer(Config config) : config_(config) {
    target_delay_ = std::clamp(config_.initial_delay_frames, config_.min_delay_frames,
                               config_.max_delay_frames);
}

void ReceiveJitterBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    nack_.clear();
    next_ext_ = 0;
    highest_ext_ = 0;
    last_timestamp_ = 0;
    have_packets_ = false;
    playout_started_ = false;
    target_delay_ = std::clamp(config_.initial_delay_frames, config_.min_delay_frames,
                               config_.max_delay_frames);
    frames_since_loss_ = 0;
    nack_round_ = 0;
    stats_ = Stats{};
}

bool ReceiveJitterBuffer::push(const RtpPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!have_packets_) {
        have_packets_ = true;
        highest_ext_ = packet.sequence;
        next_ext_ = packet.sequence;
    }

    const std::int64_t ext = extend_sequence(highest_ext_, packet.sequence);

    if (ext < next_ext_) {
        if (!playout_started_ && ext >= next_ext_ - config_.max_buffered_packets) {
            // The first packet we saw was not the first one sent. Before playout
            // starts we can still move the window back to cover it.
            next_ext_ = ext;
        } else {
            ++stats_.late;
            if (playout_started_) {
                // A packet that arrived after its playout deadline means the
                // network is jitterier than the current delay; loss alone does
                // not justify adding latency.
                increase_target();
            }
            return false;
        }
    }
    if (ext > next_ext_ + config_.max_buffered_packets) {
        ++stats_.dropped;
        return false;
    }
    if (packets_.find(ext) != packets_.end()) {
        ++stats_.duplicates;
        return false;
    }
    if (ext < highest_ext_) {
        ++stats_.reordered;
    }
    if (ext > highest_ext_) {
        highest_ext_ = ext;
    }

    packets_.emplace(ext, packet);
    nack_.erase(ext);
    ++stats_.received;
    return true;
}

ReceiveJitterBuffer::PopResult ReceiveJitterBuffer::pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++nack_round_;

    PopResult result;
    if (!have_packets_) {
        return result;
    }

    if (!playout_started_) {
        const std::int64_t span = highest_ext_ - next_ext_ + 1;
        if (span < target_delay_) {
            return result;
        }
        playout_started_ = true;
    }

    auto next = packets_.find(next_ext_);
    if (next != packets_.end()) {
        result.kind = PopKind::Frame;
        result.payload = std::move(next->second.payload);
        result.timestamp = next->second.timestamp;
        last_timestamp_ = result.timestamp;
        packets_.erase(next);
        ++next_ext_;
        ++stats_.played;
        ++frames_since_loss_;
        if (frames_since_loss_ >= config_.decrease_after_frames &&
            target_delay_ > config_.min_delay_frames) {
            --target_delay_;
            frames_since_loss_ = 0;
            ++stats_.target_decreases;
        }
        return result;
    }

    // A missing frame is only declared lost once the buffer holds enough later
    // frames that it can no longer be a reordered packet.
    if (highest_ext_ >= next_ext_ + target_delay_) {
        std::uint32_t timestamp = last_timestamp_ + static_cast<std::uint32_t>(config_.frame_samples);
        auto later = packets_.lower_bound(next_ext_);
        if (later != packets_.end()) {
            const std::int64_t ahead = later->first - next_ext_;
            timestamp = later->second.timestamp -
                        static_cast<std::uint32_t>(ahead * config_.frame_samples);
        }
        result.kind = PopKind::Concealed;
        result.timestamp = timestamp;
        last_timestamp_ = timestamp;
        ++next_ext_;
        ++stats_.concealed;
        return result;
    }

    return result;
}

void ReceiveJitterBuffer::increase_target() {
    if (target_delay_ < config_.max_delay_frames) {
        ++target_delay_;
        ++stats_.target_increases;
    }
    frames_since_loss_ = 0;
}

std::vector<std::uint16_t> ReceiveJitterBuffer::take_nack_requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint16_t> requests;
    if (!have_packets_) {
        return requests;
    }

    const std::int64_t limit =
        std::min(highest_ext_, next_ext_ + static_cast<std::int64_t>(config_.nack_window_frames));
    for (std::int64_t ext = next_ext_; ext < limit; ++ext) {
        if (packets_.find(ext) != packets_.end()) {
            nack_.erase(ext);
            continue;
        }
        NackState& state = nack_[ext];
        if (state.requests >= config_.nack_max_requests) {
            continue;
        }
        if (state.requests > 0 &&
            nack_round_ - state.last_round < static_cast<std::uint64_t>(config_.nack_retry_rounds)) {
            continue;
        }
        ++state.requests;
        state.last_round = nack_round_;
        requests.push_back(static_cast<std::uint16_t>(ext & 0xffff));
        ++stats_.nack_requests;
    }

    for (auto it = nack_.begin(); it != nack_.end();) {
        if (it->first < next_ext_ || it->first >= highest_ext_) {
            it = nack_.erase(it);
        } else {
            ++it;
        }
    }
    return requests;
}

int ReceiveJitterBuffer::target_delay_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_delay_;
}

int ReceiveJitterBuffer::buffered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(packets_.size());
}

bool ReceiveJitterBuffer::playout_started() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playout_started_;
}

ReceiveJitterBuffer::Stats ReceiveJitterBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace pk::voice
