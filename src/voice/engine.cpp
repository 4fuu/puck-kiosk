#include "voice/engine.h"

#include "core/log.h"
#include "voice/aec.h"
#include "voice/jitter_buffer.h"
#include "voice/opus_codec.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace pk::voice {
namespace {

float rms_level(const std::vector<std::int16_t>& samples) {
    if (samples.empty()) {
        return 0.0f;
    }
    double sum = 0.0;
    for (std::int16_t sample : samples) {
        const double value = static_cast<double>(sample) / 32768.0;
        sum += value * value;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

}  // namespace

struct VoiceEngine::Shared {
    explicit Shared(const ReceiveJitterBuffer::Config& config) : jitter(config) {}

    std::mutex callback_mutex;
    std::function<void(const std::string&)> data_callback;
    std::function<void(PeerState)> state_callback;
    std::atomic<PeerState> peer_state{PeerState::New};
    std::atomic<bool> connected{false};
    std::atomic<bool> muted{false};
    std::atomic<std::uint64_t> capture_frames{0};
    std::atomic<std::uint64_t> encoded_frames{0};
    std::atomic<std::uint64_t> decoded_frames{0};
    std::atomic<std::uint64_t> nack_requests{0};
    std::atomic<float> input_level{0.0f};
    std::atomic<float> output_level{0.0f};
    std::atomic<float> output_mouth{0.0f};
    ReceiveJitterBuffer jitter;
};

VoiceEngine::VoiceEngine(AudioIO& audio, VoiceEngineOptions options)
    : audio_(audio), options_(std::move(options)) {
    frame_samples_ = options_.sample_rate * options_.frame_ms / 1000;
    ReceiveJitterBuffer::Config config;
    config.clock_rate = options_.sample_rate;
    config.frame_samples = frame_samples_;
    config.initial_delay_frames = options_.jitter_initial_delay_frames;
    config.max_delay_frames = options_.jitter_max_delay_frames;
    shared_ = std::make_shared<Shared>(config);
}

VoiceEngine::~VoiceEngine() {
    stop();
}

bool VoiceEngine::create_peer(std::string* error) {
    if (peer_) {
        if (error) {
            *error = "voice peer already exists";
        }
        return false;
    }

    VoicePeerOptions peer_options;
    peer_options.test_reorder_every = options_.test_reorder_every;
    peer_options.test_drop_every = options_.test_drop_every;

    auto shared = shared_;
    peer_options.on_audio = [shared](const RtpPacket& packet) { shared->jitter.push(packet); };
    peer_options.on_data = [shared](const std::string& text) {
        std::function<void(const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(shared->callback_mutex);
            callback = shared->data_callback;
        }
        if (callback) {
            callback(text);
        }
    };
    peer_options.on_state = [shared](PeerState state) {
        shared->peer_state = state;
        shared->connected = state == PeerState::Connected;
        std::function<void(PeerState)> callback;
        {
            std::lock_guard<std::mutex> lock(shared->callback_mutex);
            callback = shared->state_callback;
        }
        if (callback) {
            callback(state);
        }
    };
    peer_options.on_error = [this](const std::string& error) { set_error(error); };

    peer_ = std::make_unique<VoicePeer>(std::move(peer_options));
    return true;
}

bool VoiceEngine::create_offer(std::string* sdp, std::string* error) {
    if (!create_peer(error)) {
        return false;
    }
    return peer_->create_offer(sdp, error);
}

bool VoiceEngine::accept_offer(const std::string& offer_sdp, std::string* answer_sdp,
                               std::string* error) {
    if (!create_peer(error)) {
        return false;
    }
    return peer_->accept_offer(offer_sdp, answer_sdp, error);
}

bool VoiceEngine::accept_answer(const std::string& answer_sdp, std::string* error) {
    if (!peer_) {
        if (error) {
            *error = "voice peer has not been created";
        }
        return false;
    }
    return peer_->accept_answer(answer_sdp, error);
}

bool VoiceEngine::start_audio(std::string* error) {
    if (audio_started_) {
        return true;
    }
    if (options_.sample_rate != 48000 || options_.channels != 1 || options_.frame_ms != 20) {
        if (error) {
            *error = "voice audio is fixed at 48 kHz mono with 20 ms frames";
        }
        return false;
    }
    if (frame_samples_ != 960) {
        if (error) {
            *error = "voice frame size is not 960 samples";
        }
        return false;
    }

    encoder_ = std::make_unique<OpusEncoder>();
    if (!encoder_->initialize(options_.sample_rate, options_.channels, options_.opus_bitrate, error)) {
        encoder_.reset();
        return false;
    }
    decoder_ = std::make_unique<OpusDecoder>();
    if (!decoder_->initialize(options_.sample_rate, options_.channels, error)) {
        encoder_.reset();
        decoder_.reset();
        return false;
    }

    if (options_.enable_aec) {
        aec_ = std::make_unique<EchoCanceller>();
        std::string aec_error;
        if (!aec_->initialize(options_.sample_rate, options_.channels, &aec_error)) {
            log::warn("voice: AEC unavailable (%s); continuing without it", aec_error.c_str());
            aec_.reset();
        }
    }

    if (!audio_.start_capture(options_.sample_rate, options_.channels, error)) {
        encoder_.reset();
        decoder_.reset();
        aec_.reset();
        return false;
    }
    if (!audio_.start_playback(options_.sample_rate, options_.channels, error)) {
        audio_.stop_capture();
        encoder_.reset();
        decoder_.reset();
        aec_.reset();
        return false;
    }

    running_ = true;
    try {
        capture_thread_ = std::thread(&VoiceEngine::capture_loop, this);
        playback_thread_ = std::thread(&VoiceEngine::playback_loop, this);
    } catch (const std::exception& exception) {
        running_ = false;
        audio_.stop_capture();
        audio_.stop_playback();
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        encoder_.reset();
        decoder_.reset();
        aec_.reset();
        if (error) {
            *error = std::string("cannot start voice threads: ") + exception.what();
        }
        return false;
    }
    audio_started_ = true;
    log::info("voice: audio started on %s (AEC %s)", audio_.name(), aec_ ? "on" : "off");
    return true;
}

void VoiceEngine::stop_audio() {
    if (!audio_started_.exchange(false) && !running_.load()) {
        return;
    }
    running_ = false;
    audio_.stop_capture();
    audio_.stop_playback();
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    encoder_.reset();
    decoder_.reset();
    aec_.reset();
}

void VoiceEngine::stop() {
    stop_audio();
    if (peer_) {
        peer_->close();
        peer_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(shared_->callback_mutex);
        shared_->data_callback = nullptr;
        shared_->state_callback = nullptr;
    }
}

void VoiceEngine::capture_loop() {
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_samples_));
    std::vector<std::uint8_t> payload;
    std::string codec_error;

    while (running_) {
        const std::size_t got = audio_.read_capture(pcm.data(), pcm.size());
        if (!running_) {
            break;
        }
        if (got == 0) {
            continue;
        }
        if (got < pcm.size()) {
            std::fill(pcm.begin() + static_cast<std::ptrdiff_t>(got), pcm.end(), 0);
        }

        if (aec_) {
            aec_->process_capture(pcm.data(), frame_samples_);
        }
        const float level = rms_level(pcm);
        shared_->input_level = shared_->muted ? 0.0f : level;
        if (shared_->muted) {
            std::fill(pcm.begin(), pcm.end(), 0);
        }

        const int encoded = encoder_->encode(pcm.data(), frame_samples_, &payload, &codec_error);
        if (encoded <= 0) {
            set_error("Opus encode: " + codec_error);
            continue;
        }
        ++shared_->encoded_frames;
        ++shared_->capture_frames;

        if (peer_ && peer_->send_audio(payload.data(), payload.size(), rtp_timestamp_)) {
            // sent_packets is exposed by the peer itself.
        }
        rtp_timestamp_ += static_cast<std::uint32_t>(frame_samples_);
    }
}

void VoiceEngine::playback_loop() {
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_samples_));
    std::string codec_error;
    const auto frame_duration =
        std::chrono::microseconds(static_cast<long long>(1000000LL * frame_samples_ / options_.sample_rate));
    auto next = std::chrono::steady_clock::now();

    while (running_) {
        const ReceiveJitterBuffer::PopResult result = shared_->jitter.pop();
        std::fill(pcm.begin(), pcm.end(), 0);
        if (result.kind == ReceiveJitterBuffer::PopKind::Frame) {
            const int decoded =
                decoder_->decode(result.payload.data(), result.payload.size(), pcm.data(),
                                 frame_samples_, &codec_error);
            if (decoded > 0) {
                ++shared_->decoded_frames;
            } else {
                set_error("Opus decode: " + codec_error);
            }
        } else if (result.kind == ReceiveJitterBuffer::PopKind::Concealed) {
            decoder_->decode_plc(pcm.data(), frame_samples_, &codec_error);
        }

        if (aec_) {
            aec_->process_render(pcm.data(), frame_samples_);
        }
        audio_.write_playback(pcm.data(), pcm.size());

        const float level = rms_level(pcm);
        const float held = std::max(level, shared_->output_level.load() * 0.85f);
        shared_->output_level = held;
        const float target = std::min(1.0f, held * 6.0f);
        float mouth = shared_->output_mouth.load();
        mouth += (target - mouth) * 0.35f;
        shared_->output_mouth = mouth;

        const std::vector<std::uint16_t> missing = shared_->jitter.take_nack_requests();
        if (!missing.empty() && peer_) {
            if (peer_->send_nack(missing)) {
                shared_->nack_requests += missing.size();
            }
        }

        next += frame_duration;
        const auto now = std::chrono::steady_clock::now();
        if (next > now) {
            std::this_thread::sleep_until(next);
        } else if (now - next > frame_duration * 5) {
            next = now;
        }
    }
}

void VoiceEngine::set_muted(bool muted) {
    shared_->muted = muted;
}

bool VoiceEngine::muted() const {
    return shared_->muted.load();
}

bool VoiceEngine::send_data(const std::string& text) {
    return peer_ && peer_->send_data(text);
}

void VoiceEngine::set_data_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(shared_->callback_mutex);
    shared_->data_callback = std::move(callback);
}

void VoiceEngine::set_peer_state_callback(std::function<void(PeerState)> callback) {
    std::lock_guard<std::mutex> lock(shared_->callback_mutex);
    shared_->state_callback = std::move(callback);
}

bool VoiceEngine::connected() const {
    return shared_->connected.load();
}

PeerState VoiceEngine::peer_state() const {
    return shared_->peer_state.load();
}

VoiceEngineStats VoiceEngine::stats() const {
    VoiceEngineStats stats;
    const ReceiveJitterBuffer::Stats jitter = shared_->jitter.stats();
    stats.received_packets = jitter.received;
    stats.concealed_frames = jitter.concealed;
    stats.late_packets = jitter.late;
    stats.reordered_packets = jitter.reordered;
    stats.duplicate_packets = jitter.duplicates;
    stats.jitter_target_frames = shared_->jitter.target_delay_frames();
    stats.jitter_buffered_frames = shared_->jitter.buffered_frames();
    stats.capture_frames = shared_->capture_frames.load();
    stats.encoded_frames = shared_->encoded_frames.load();
    stats.decoded_frames = shared_->decoded_frames.load();
    stats.nack_requests = shared_->nack_requests.load();
    stats.input_level = shared_->input_level.load();
    stats.output_level = shared_->output_level.load();
    stats.output_mouth = shared_->output_mouth.load();
    stats.connected = shared_->connected.load();
    stats.peer_state = shared_->peer_state.load();
    if (peer_) {
        stats.sent_packets = peer_->sent_packets();
        stats.retransmitted_packets = peer_->retransmitted_packets();
        stats.dropped_packets = peer_->dropped_packets();
    }
    return stats;
}

std::string VoiceEngine::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

const char* VoiceEngine::audio_name() const {
    return audio_.name();
}

void VoiceEngine::set_error(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = error;
    }
    log::warn("voice: %s", error.c_str());
}

}  // namespace pk::voice
