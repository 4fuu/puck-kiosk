#include "voice/aec.h"

#include <algorithm>
#include <cmath>
#include <vector>

#if PUCK_KIOSK_ENABLE_AEC
#include "modules/audio_processing/include/audio_processing.h"
#endif

namespace pk::voice {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

EchoCanceller::EchoCanceller() = default;

EchoCanceller::~EchoCanceller() {
#if PUCK_KIOSK_ENABLE_AEC
    delete static_cast<webrtc::AudioProcessing*>(apm_);
#endif
    apm_ = nullptr;
}

bool EchoCanceller::initialize(int sample_rate, int channels, std::string* error) {
#if PUCK_KIOSK_ENABLE_AEC
    if (sample_rate % 100 != 0 || channels != 1) {
        if (error) {
            *error = "AEC requires mono audio with a sample rate divisible by 100";
        }
        return false;
    }

    webrtc::AudioProcessingBuilder builder;
    webrtc::AudioProcessing* apm = builder.Create();
    if (!apm) {
        if (error) {
            *error = "webrtc-audio-processing could not create an AudioProcessing instance";
        }
        return false;
    }

    webrtc::AudioProcessing::Config config;
    config.pipeline.maximum_internal_processing_rate = 48000;
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false;
    config.noise_suppression.enabled = true;
    config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
    config.high_pass_filter.enabled = true;
    config.gain_controller1.enabled = false;
    config.gain_controller2.enabled = false;
    apm->ApplyConfig(config);

    apm_ = apm;
    sample_rate_ = sample_rate;
    channels_ = channels;
    return true;
#else
    (void)sample_rate;
    (void)channels;
    if (error) {
        *error = "built without webrtc-audio-processing";
    }
    return false;
#endif
}

void EchoCanceller::process_render(const std::int16_t* samples, int frames) {
#if PUCK_KIOSK_ENABLE_AEC
    if (!apm_ || !samples || frames <= 0) {
        return;
    }
    auto* apm = static_cast<webrtc::AudioProcessing*>(apm_);
    const int chunk = sample_rate_ / 100;
    std::vector<std::int16_t> scratch(static_cast<std::size_t>(chunk));
    std::lock_guard<std::mutex> lock(mutex_);
    for (int offset = 0; offset + chunk <= frames; offset += chunk) {
        webrtc::StreamConfig config(sample_rate_, static_cast<std::size_t>(channels_));
        apm->ProcessReverseStream(samples + offset, config, config, scratch.data());
    }
#else
    (void)samples;
    (void)frames;
#endif
}

void EchoCanceller::process_capture(std::int16_t* samples, int frames) {
#if PUCK_KIOSK_ENABLE_AEC
    if (!apm_ || !samples || frames <= 0) {
        return;
    }
    auto* apm = static_cast<webrtc::AudioProcessing*>(apm_);
    const int chunk = sample_rate_ / 100;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int offset = 0; offset + chunk <= frames; offset += chunk) {
        webrtc::StreamConfig config(sample_rate_, static_cast<std::size_t>(channels_));
        apm->ProcessStream(samples + offset, config, config, samples + offset);
    }
#else
    (void)samples;
    (void)frames;
#endif
}

bool run_aec_smoke_test(int seconds, double* erle_db, std::string* error) {
#if PUCK_KIOSK_ENABLE_AEC
    if (seconds < 2) {
        seconds = 2;
    }
    EchoCanceller aec;
    if (!aec.initialize(48000, 1, error)) {
        return false;
    }

    constexpr int kSampleRate = 48000;
    constexpr int kFrame = 960;
    constexpr int kDelay = 960;
    const std::size_t total = static_cast<std::size_t>(seconds) * kSampleRate;
    std::vector<std::int16_t> render(total, 0);
    std::vector<std::int16_t> capture(total, 0);
    std::vector<std::int16_t> output(total, 0);

    for (std::size_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double envelope = 0.65 + 0.35 * std::sin(2.0 * kPi * 2.5 * t);
        const double far = 0.28 * envelope * std::sin(2.0 * kPi * 440.0 * t);
        render[i] = static_cast<std::int16_t>(far * 32767.0);
        if (i >= kDelay) {
            capture[i] = static_cast<std::int16_t>(render[i - kDelay] * 0.5);
        }
        // A tiny independent noise floor keeps the AEC estimator honest.
        const double noise = 0.002 * std::sin(2.0 * kPi * 1733.0 * t);
        capture[i] = static_cast<std::int16_t>(
            std::clamp(static_cast<double>(capture[i]) + noise * 32767.0, -32768.0, 32767.0));
    }

    for (std::size_t offset = 0; offset + kFrame <= total; offset += kFrame) {
        aec.process_render(render.data() + offset, kFrame);
        std::copy(capture.begin() + static_cast<std::ptrdiff_t>(offset),
                  capture.begin() + static_cast<std::ptrdiff_t>(offset + kFrame),
                  output.begin() + static_cast<std::ptrdiff_t>(offset));
        aec.process_capture(output.data() + offset, kFrame);
    }

    const std::size_t measure_from = total / 2;
    double input_energy = 0.0;
    double output_energy = 0.0;
    for (std::size_t i = measure_from; i < total; ++i) {
        input_energy += static_cast<double>(capture[i]) * capture[i];
        output_energy += static_cast<double>(output[i]) * output[i];
    }
    if (input_energy <= 0.0) {
        if (error) {
            *error = "AEC smoke input was silent";
        }
        return false;
    }
    if (output_energy <= 1.0) {
        *erle_db = 99.0;
    } else {
        *erle_db = 10.0 * std::log10(input_energy / output_energy);
    }
    return std::isfinite(*erle_db);
#else
    (void)seconds;
    (void)erle_db;
    if (error) {
        *error = "built without webrtc-audio-processing";
    }
    return false;
#endif
}

}  // namespace pk::voice
