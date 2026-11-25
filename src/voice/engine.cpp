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
    std::atomic<bool> input_silenced{false};
    std::atomic<bool> chime_requested{false};
    std::atomic<std::uint64_t> capture_frames{0};
    std::atomic<std::uint64_t> encoded_frames{0};
    std::atomic<std::uint64_t> decoded_frames{0};
    std::atomic<std::uint64_t> nack_requests{0};
    std::atomic<std::uint64_t> ready_chimes{0};
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
    if (stop_requested_.load()) {
        if (error) {
            *error = "voice engine is stopped";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(peer_mutex_);
    if (stop_requested_.load()) {
        if (error) {
            *error = "voice engine is stopped";
        }
        return false;
    }
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
        // Invoke while holding the lock so clearing the callback cannot leave
        // an in-flight call using a destroyed controller.
        std::lock_guard<std::mutex> lock(shared->callback_mutex);
        if (shared->data_callback) {
            shared->data_callback(text);
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

    peer_ = std::make_shared<VoicePeer>(std::move(peer_options));
    return true;
}

std::shared_ptr<VoicePeer> VoiceEngine::peer_snapshot() const {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return peer_;
}

bool VoiceEngine::create_offer(std::string* sdp, std::string* error) {
    if (!create_peer(error)) {
        return false;
    }
    const std::shared_ptr<VoicePeer> peer = peer_snapshot();
    if (!peer) {
        if (error) {
            *error = "voice peer was not created";
        }
        return false;
    }
    return peer->create_offer(sdp, error);
}

bool VoiceEngine::accept_offer(const std::string& offer_sdp, std::string* answer_sdp,
                               std::string* error) {
    if (!create_peer(error)) {
        return false;
    }
    const std::shared_ptr<VoicePeer> peer = peer_snapshot();
    if (!peer) {
        if (error) {
            *error = "voice peer was not created";
        }
        return false;
    }
    return peer->accept_offer(offer_sdp, answer_sdp, error);
}

bool VoiceEngine::accept_answer(const std::string& answer_sdp, std::string* error) {
    const std::shared_ptr<VoicePeer> peer = peer_snapshot();
    if (!peer) {
        if (error) {
            *error = "voice peer has not been created";
        }
        return false;
    }
    return peer->accept_answer(answer_sdp, error);
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
            std::lock_guard<std::mutex> lock(aec_mutex_);
            aec_status_ = AecStatus{};
            aec_status_.detail = aec_error;
        } else {
            std::lock_guard<std::mutex> lock(aec_mutex_);
            aec_status_ = aec_->status();
        }
    } else {
        std::lock_guard<std::mutex> lock(aec_mutex_);
        aec_status_ = AecStatus{};
        aec_status_.detail = "disabled for this call";
    }

    std::string capture_error;
    if (!audio_.start_capture(options_.sample_rate, options_.channels, &capture_error)) {
        if (error) {
            *error = "microphone unavailable: " + capture_error;
        }
        encoder_.reset();
        decoder_.reset();
        aec_.reset();
        std::lock_guard<std::mutex> lock(aec_mutex_);
        aec_status_ = AecStatus{};
        aec_status_.detail = "not running: microphone unavailable";
        return false;
    }
    std::string playback_error;
    if (!audio_.start_playback(options_.sample_rate, options_.channels, &playback_error)) {
        if (error) {
            *error = "speaker unavailable: " + playback_error;
        }
        audio_.stop_capture();
        encoder_.reset();
        decoder_.reset();
        aec_.reset();
        std::lock_guard<std::mutex> lock(aec_mutex_);
        aec_status_ = AecStatus{};
        aec_status_.detail = "not running: speaker unavailable";
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
        {
            std::lock_guard<std::mutex> lock(aec_mutex_);
            aec_status_ = AecStatus{};
            aec_status_.detail = "not running: voice threads failed";
        }
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
    shared_->chime_requested = false;
    shared_->input_level = 0.0f;
    shared_->output_level = 0.0f;
    shared_->output_mouth = 0.0f;
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
    shared_->input_level = 0.0f;
    shared_->output_level = 0.0f;
    shared_->output_mouth = 0.0f;
    shared_->chime_requested = false;
    encoder_.reset();
    decoder_.reset();
    aec_.reset();
    {
        std::lock_guard<std::mutex> lock(aec_mutex_);
        aec_status_ = AecStatus{};
        aec_status_.detail = "not running";
    }
}

voice::AecStatus VoiceEngine::aec_status() const {
    std::lock_guard<std::mutex> lock(aec_mutex_);
    return aec_status_;
}

void VoiceEngine::stop() {
    stop_requested_ = true;
    stop_audio();
    std::shared_ptr<VoicePeer> peer;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer = std::move(peer_);
    }
    if (peer) {
        peer->close();
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
        const bool silenced = shared_->input_silenced.load();
        const float level = std::min(1.0f, rms_level(pcm) * 6.0f);
        shared_->input_level = silenced ? 0.0f : level;
        if (silenced) {
            std::fill(pcm.begin(), pcm.end(), 0);
        }

        const int encoded = encoder_->encode(pcm.data(), frame_samples_, &payload, &codec_error);
        if (encoded <= 0) {
            set_error("Opus encode: " + codec_error);
            continue;
        }
        ++shared_->encoded_frames;
        ++shared_->capture_frames;

        if (const std::shared_ptr<VoicePeer> peer = peer_snapshot()) {
            peer->send_audio(payload.data(), payload.size(), rtp_timestamp_);
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

    std::vector<std::int16_t> ready_chime;
    const int chime_frames = options_.sample_rate * 300 / 1000;
    ready_chime.resize(static_cast<std::size_t>(chime_frames));
    constexpr double kTwoPi = 6.28318530717958647692;
    double chime_phase = 0.0;
    for (int i = 0; i < chime_frames; ++i) {
        const double t = static_cast<double>(i) / options_.sample_rate;
        const double frequency = t < 0.14 ? 880.0 : 1320.0;
        double envelope = 1.0;
        if (t < 0.01) {
            envelope = t / 0.01;
        } else if (t > 0.26) {
            envelope = (0.30 - t) / 0.04;
        }
        const double value = std::sin(chime_phase) * 0.12 * std::max(0.0, envelope);
        ready_chime[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(value * 32767.0);
        chime_phase += kTwoPi * frequency / options_.sample_rate;
    }
    bool chime_active = false;
    std::size_t chime_pos = 0;

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

        // Web output metering: RMS x4, limited to 1, before the local cue is
        // mixed in, so the ready chime never drives the speaking visuals.
        const float level = std::min(1.0f, rms_level(pcm) * 4.0f);
        shared_->output_level = level;
        const float target = std::min(1.0f, level * 2.0f);
        float mouth = shared_->output_mouth.load();
        mouth += (target - mouth) * 0.35f;
        shared_->output_mouth = mouth;

        if (!ready_chime.empty() && shared_->chime_requested.exchange(false)) {
            chime_active = true;
            chime_pos = 0;
            ++shared_->ready_chimes;
        }
        if (chime_active) {
            const std::size_t count =
                std::min<std::size_t>(pcm.size(), ready_chime.size() - chime_pos);
            for (std::size_t i = 0; i < count; ++i) {
                const int mixed = static_cast<int>(pcm[i]) +
                                  static_cast<int>(ready_chime[chime_pos + i]);
                pcm[i] = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
            }
            chime_pos += count;
            if (chime_pos >= ready_chime.size()) {
                chime_active = false;
            }
        }

        if (aec_) {
            aec_->process_render(pcm.data(), frame_samples_);
        }
        audio_.write_playback(pcm.data(), pcm.size());

        const std::vector<std::uint16_t> missing = shared_->jitter.take_nack_requests();
        if (!missing.empty()) {
            if (const std::shared_ptr<VoicePeer> peer = peer_snapshot()) {
                if (peer->send_nack(missing)) {
                    shared_->nack_requests += missing.size();
                }
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

void VoiceEngine::set_input_silenced(bool silenced) {
    shared_->input_silenced = silenced;
    if (silenced) {
        shared_->input_level = 0.0f;
    }
}

void VoiceEngine::play_ready_chime() {
    if (audio_started_.load()) {
        shared_->chime_requested = true;
    }
}

bool VoiceEngine::send_data(const std::string& text) {
    const std::shared_ptr<VoicePeer> peer = peer_snapshot();
    return peer && peer->send_data(text);
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
    stats.ready_chimes = shared_->ready_chimes.load();
    stats.input_level = shared_->input_level.load();
    stats.output_level = shared_->output_level.load();
    stats.output_mouth = shared_->output_mouth.load();
    stats.connected = shared_->connected.load();
    stats.peer_state = shared_->peer_state.load();
    if (const std::shared_ptr<VoicePeer> peer = peer_snapshot()) {
        stats.sent_packets = peer->sent_packets();
        stats.retransmitted_packets = peer->retransmitted_packets();
        stats.dropped_packets = peer->dropped_packets();
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
