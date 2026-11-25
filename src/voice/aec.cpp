#include "voice/aec.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
#include "modules/audio_processing/include/audio_processing.h"
#endif

#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#endif

namespace pk::voice {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlockMs = 10;

bool active(const AecSettings& settings) {
    return settings.echo_cancellation || settings.noise_suppression || settings.automatic_gain;
}

void set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
webrtc::AudioProcessing::Config apm_config(const AecSettings& settings) {
    webrtc::AudioProcessing::Config config;
    config.pipeline.maximum_internal_processing_rate = 48000;
    config.echo_canceller.enabled = settings.echo_cancellation;
    config.echo_canceller.mobile_mode = false;
    config.noise_suppression.enabled = settings.noise_suppression;
    config.noise_suppression.level =
        webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
    config.high_pass_filter.enabled = settings.high_pass_filter;
    config.gain_controller1.enabled = settings.automatic_gain;
    // Digital AGC takes the capture level only; it never asks the device for
    // analog gain, which WASAPI cannot provide through the shared endpoint.
    config.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
    config.gain_controller2.enabled = false;
    return config;
}
#endif

#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
void portable_control(SpeexPreprocessState* pre, int request, int value) {
    speex_preprocess_ctl(pre, request, &value);
}
#endif

}  // namespace

struct EchoCanceller::Impl {
    AecSettings settings;
    AecStatus status;
    int sample_rate = 0;
    int chunk = 0;
    std::mutex mutex;

#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
    webrtc::AudioProcessing* apm = nullptr;
    std::vector<std::int16_t> apm_scratch;
    std::vector<std::int16_t> apm_reverse_scratch;
#endif

#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
    SpeexEchoState* echo = nullptr;
    SpeexPreprocessState* pre = nullptr;
    std::size_t delay_chunks = 0;
    std::deque<std::vector<std::int16_t>> render_delay;
    std::vector<std::int16_t> portable_capture;
    std::vector<std::int16_t> portable_output;
    std::vector<std::int16_t> portable_silence;
    double high_pass_x1 = 0.0;
    double high_pass_y1 = 0.0;
#endif

    ~Impl() { reset(); }

    void reset() {
#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
        delete apm;
        apm = nullptr;
        apm_scratch.clear();
        apm_reverse_scratch.clear();
#endif
#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
        if (pre) {
            speex_preprocess_state_destroy(pre);
            pre = nullptr;
        }
        if (echo) {
            speex_echo_state_destroy(echo);
            echo = nullptr;
        }
        render_delay.clear();
        delay_chunks = 0;
        portable_capture.clear();
        portable_output.clear();
        portable_silence.clear();
        high_pass_x1 = 0.0;
        high_pass_y1 = 0.0;
#endif
        status = AecStatus{};
        sample_rate = 0;
        chunk = 0;
    }
};

EchoCanceller::EchoCanceller() : impl_(std::make_unique<Impl>()) {}

EchoCanceller::~EchoCanceller() = default;

bool EchoCanceller::initialize(int sample_rate, int channels, std::string* error) {
    return initialize(sample_rate, channels, AecSettings{}, error);
}

bool EchoCanceller::initialize(int sample_rate, int channels, const AecSettings& settings,
                               std::string* error) {
    if (impl_->status.initialized) {
        set_error(error, "AEC is already initialized");
        return false;
    }
    if (channels != 1) {
        set_error(error, "AEC requires mono audio");
        return false;
    }
    if (sample_rate < 8000 || sample_rate > 48000 || sample_rate % 100 != 0) {
        set_error(error, "AEC requires a sample rate from 8 kHz to 48 kHz in 100 Hz steps");
        return false;
    }

    impl_->settings = settings;
    impl_->sample_rate = sample_rate;
    impl_->chunk = sample_rate / 100;

    if (!active(settings)) {
        impl_->status.initialized = true;
        impl_->status.backend = "none";
        impl_->status.detail = "bypass";
        impl_->status.sample_rate = sample_rate;
        return true;
    }

    const AecCapabilities capabilities = aec_capabilities();
    const bool try_webrtc = (settings.backend == AecBackend::Auto ||
                             settings.backend == AecBackend::WebRtc) &&
                            capabilities.webrtc;
    const bool try_portable = (settings.backend == AecBackend::Auto ||
                               settings.backend == AecBackend::Portable) &&
                              capabilities.portable;

    if (!try_webrtc && !try_portable) {
        if (settings.backend == AecBackend::WebRtc) {
            set_error(error, "this build has no webrtc-audio-processing backend");
        } else if (settings.backend == AecBackend::Portable) {
            set_error(error, "this build has no portable AEC backend");
        } else {
            set_error(error, "this build has no AEC backend");
        }
        return false;
    }

    std::string first_error;
    if (try_webrtc) {
#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
        webrtc::AudioProcessingBuilder builder;
        webrtc::AudioProcessing* apm = builder.Create();
        if (apm) {
            apm->ApplyConfig(apm_config(settings));
            impl_->apm = apm;
            impl_->apm_scratch.assign(static_cast<std::size_t>(impl_->chunk), 0);
            impl_->apm_reverse_scratch.assign(static_cast<std::size_t>(impl_->chunk), 0);
            impl_->status.initialized = true;
            impl_->status.echo_cancellation = settings.echo_cancellation;
            impl_->status.noise_suppression = settings.noise_suppression;
            impl_->status.automatic_gain = settings.automatic_gain;
            impl_->status.backend = "webrtc-audio-processing";
            impl_->status.sample_rate = sample_rate;
            impl_->status.frame_ms = kBlockMs;
            impl_->status.tail_ms = 0;
            impl_->status.detail = "WebRTC APM (internal delay estimation)";
            return true;
        }
        first_error = "webrtc-audio-processing could not create an AudioProcessing instance";
#else
        first_error = "this build has no webrtc-audio-processing backend";
#endif
    }

    if (try_portable) {
#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
        const int tail = std::max(impl_->chunk,
                                  settings.tail_ms * sample_rate / 1000 / impl_->chunk *
                                      impl_->chunk);
        SpeexEchoState* echo = nullptr;
        SpeexPreprocessState* pre = nullptr;
        if (settings.echo_cancellation) {
            echo = speex_echo_state_init(impl_->chunk, tail);
            if (!echo) {
                set_error(error, "speexdsp could not create the echo canceller");
                return false;
            }
            int rate = sample_rate;
            speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        }
        pre = speex_preprocess_state_init(impl_->chunk, sample_rate);
        if (!pre) {
            if (echo) {
                speex_echo_state_destroy(echo);
            }
            set_error(error, "speexdsp could not create the preprocessor");
            return false;
        }
        if (echo) {
            speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
        }
        portable_control(pre, SPEEX_PREPROCESS_SET_DENOISE,
                         settings.noise_suppression ? 1 : 0);
        portable_control(pre, SPEEX_PREPROCESS_SET_AGC,
                         settings.automatic_gain ? 1 : 0);
        portable_control(pre, SPEEX_PREPROCESS_SET_VAD, 0);
        portable_control(pre, SPEEX_PREPROCESS_SET_DEREVERB, 0);
        portable_control(pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, -25);
        portable_control(pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, -35);
        portable_control(pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, -12);
        if (settings.automatic_gain) {
            // SET_AGC_TARGET takes spx_int32_t*, not float. Passing 8000.0f
            // here made SpeexDSP read the float's bit pattern and clamp the
            // target to 32768, the maximum.
            portable_control(pre, SPEEX_PREPROCESS_SET_AGC_TARGET, 8000);
        }
        impl_->echo = echo;
        impl_->pre = pre;
        impl_->portable_capture.assign(static_cast<std::size_t>(impl_->chunk), 0);
        impl_->portable_output.assign(static_cast<std::size_t>(impl_->chunk), 0);
        impl_->portable_silence.assign(static_cast<std::size_t>(impl_->chunk), 0);
        impl_->delay_chunks = static_cast<std::size_t>(
            std::max(0, settings.reference_delay_ms) / kBlockMs);
        for (std::size_t i = 0; i < impl_->delay_chunks; ++i) {
            impl_->render_delay.emplace_back(impl_->portable_silence);
        }
        impl_->status.initialized = true;
        impl_->status.echo_cancellation = settings.echo_cancellation;
        impl_->status.noise_suppression = settings.noise_suppression;
        impl_->status.automatic_gain = settings.automatic_gain;
        impl_->status.backend = "speexdsp";
        impl_->status.detail =
            first_error.empty() ? "portable backend" : "fallback: " + first_error;
        impl_->status.sample_rate = sample_rate;
        impl_->status.frame_ms = kBlockMs;
        impl_->status.tail_ms = settings.tail_ms;
        return true;
#else
        if (first_error.empty()) {
            first_error = "this build has no portable AEC backend";
        }
#endif
    }

    set_error(error, first_error.empty() ? "no AEC backend could be initialized" : first_error);
    impl_->reset();
    return false;
}

bool EchoCanceller::enabled() const {
    return impl_ && impl_->status.initialized &&
           (impl_->status.echo_cancellation || impl_->status.noise_suppression ||
            impl_->status.automatic_gain);
}

const AecStatus& EchoCanceller::status() const {
    return impl_->status;
}

void EchoCanceller::process_render(const std::int16_t* samples, int frames) {
    if (!impl_ || !samples || frames <= 0 || !enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
    if (impl_->apm) {
        webrtc::StreamConfig config(impl_->sample_rate, 1);
        for (int offset = 0; offset + impl_->chunk <= frames; offset += impl_->chunk) {
            impl_->apm->ProcessReverseStream(samples + offset, config, config,
                                             impl_->apm_reverse_scratch.data());
        }
        return;
    }
#endif
#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
    if (impl_->echo) {
        for (int offset = 0; offset + impl_->chunk <= frames; offset += impl_->chunk) {
            if (impl_->delay_chunks == 0) {
                speex_echo_playback(impl_->echo, samples + offset);
                continue;
            }
            impl_->render_delay.emplace_back(samples + offset,
                                             samples + offset + impl_->chunk);
            if (impl_->render_delay.size() > impl_->delay_chunks) {
                speex_echo_playback(impl_->echo, impl_->render_delay.front().data());
                impl_->render_delay.pop_front();
            }
        }
    }
#else
    (void)samples;
    (void)frames;
#endif
}

void EchoCanceller::process_capture(std::int16_t* samples, int frames) {
    if (!impl_ || !samples || frames <= 0 || !enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(PUCK_KIOSK_ENABLE_AEC) && PUCK_KIOSK_ENABLE_AEC
    if (impl_->apm) {
        webrtc::StreamConfig config(impl_->sample_rate, 1);
        for (int offset = 0; offset + impl_->chunk <= frames; offset += impl_->chunk) {
            // WebRTC's int16 path does not promise in-place operation; the
            // separate buffer also keeps a partially filled frame intact.
            if (impl_->apm->ProcessStream(samples + offset, config, config,
                                          impl_->apm_scratch.data()) == 0) {
                std::memcpy(samples + offset, impl_->apm_scratch.data(),
                            sizeof(std::int16_t) * static_cast<std::size_t>(impl_->chunk));
            }
        }
        return;
    }
#endif
#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC) && PUCK_KIOSK_ENABLE_PORTABLE_AEC
    if (impl_->pre) {
        for (int offset = 0; offset + impl_->chunk <= frames; offset += impl_->chunk) {
            std::memcpy(impl_->portable_capture.data(), samples + offset,
                        sizeof(std::int16_t) * static_cast<std::size_t>(impl_->chunk));
            if (impl_->echo) {
                speex_echo_capture(impl_->echo, impl_->portable_capture.data(),
                                   impl_->portable_output.data());
            } else {
                std::copy(impl_->portable_capture.begin(), impl_->portable_capture.end(),
                          impl_->portable_output.begin());
            }
            speex_preprocess_run(impl_->pre, impl_->portable_output.data());
            if (impl_->settings.high_pass_filter) {
                // One-pole DC blocker; SpeexDSP has no high-pass stage. It
                // only removes DC and sub-audio rumble, not speech band.
                for (int i = 0; i < impl_->chunk; ++i) {
                    const double x = impl_->portable_output[static_cast<std::size_t>(i)];
                    const double y = x - impl_->high_pass_x1 + 0.995 * impl_->high_pass_y1;
                    impl_->high_pass_x1 = x;
                    impl_->high_pass_y1 = y;
                    impl_->portable_output[static_cast<std::size_t>(i)] =
                        static_cast<std::int16_t>(std::clamp(y, -32768.0, 32767.0));
                }
            }
            std::memcpy(samples + offset, impl_->portable_output.data(),
                        sizeof(std::int16_t) * static_cast<std::size_t>(impl_->chunk));
        }
    }
#else
    (void)samples;
    (void)frames;
#endif
}

bool run_aec_smoke_test(int seconds, AecBackend backend, double* erle_db,
                        std::string* error) {
    if (seconds < 2) {
        seconds = 2;
    }
    constexpr int sample_rate = 48000;
    constexpr int frame = 960;
    const int total = seconds * sample_rate;
    std::vector<std::int16_t> render(static_cast<std::size_t>(total), 0);
    std::vector<std::int16_t> capture(static_cast<std::size_t>(total), 0);
    std::vector<std::int16_t> output(static_cast<std::size_t>(total), 0);

    // A speech-like harmonic signal, not a single tone: the AEC has to track
    // pitch and amplitude changes as it does for Puck's voice.
    for (int i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        const double envelope = 0.35 + 0.65 * (0.5 + 0.5 * std::sin(2.0 * kPi * 2.3 * t));
        const double f0 = 180.0 + 35.0 * std::sin(2.0 * kPi * 0.7 * t);
        double value = 0.0;
        for (int harmonic = 1; harmonic <= 6; ++harmonic) {
            value += std::sin(2.0 * kPi * f0 * harmonic * t) / harmonic;
        }
        value *= 0.16 * envelope;
        render[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(std::clamp(value, -1.0, 1.0) * 32767.0);
        const int echo_at = i - 37 * sample_rate / 1000;
        if (echo_at >= 0) {
            double echo = 0.55 * static_cast<double>(render[static_cast<std::size_t>(echo_at)]);
            const int reflection = i - 68 * sample_rate / 1000;
            if (reflection >= 0) {
                echo += 0.12 * static_cast<double>(render[static_cast<std::size_t>(reflection)]);
            }
            capture[static_cast<std::size_t>(i)] =
                static_cast<std::int16_t>(std::clamp(echo, -32768.0, 32767.0));
        }
    }

    EchoCanceller aec;
    AecSettings settings;
    settings.tail_ms = 200;
    settings.backend = backend;
    if (!aec.initialize(sample_rate, 1, settings, error)) {
        return false;
    }
    for (int offset = 0; offset + frame <= total; offset += frame) {
        aec.process_render(render.data() + offset, frame);
        std::memcpy(output.data() + offset, capture.data() + offset,
                    sizeof(std::int16_t) * static_cast<std::size_t>(frame));
        aec.process_capture(output.data() + offset, frame);
    }

    const int measure_from = total / 2;
    double input_energy = 0.0;
    double output_energy = 0.0;
    for (int i = measure_from; i < total; ++i) {
        input_energy += static_cast<double>(capture[static_cast<std::size_t>(i)]) *
                        capture[static_cast<std::size_t>(i)];
        output_energy += static_cast<double>(output[static_cast<std::size_t>(i)]) *
                         output[static_cast<std::size_t>(i)];
    }
    if (input_energy <= 0.0) {
        set_error(error, "AEC smoke input was silent");
        return false;
    }
    *erle_db = output_energy <= 1.0
                   ? 99.0
                   : 10.0 * std::log10(input_energy / output_energy);
    log::info("AEC smoke: backend %s, ERLE %.1f dB", aec.status().backend.c_str(), *erle_db);
    if (!std::isfinite(*erle_db) || *erle_db < 6.0) {
        set_error(error, "AEC smoke did not reduce the echo");
        return false;
    }
    return true;
}

bool run_aec_smoke_test(int seconds, double* erle_db, std::string* error) {
    return run_aec_smoke_test(seconds, AecBackend::Auto, erle_db, error);
}

}  // namespace pk::voice
