#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pk::voice {

// Compile-time view of the backends in this binary. Diagnostics uses it
// without linking the audio-processing implementation.
struct AecCapabilities {
    bool built = false;
    bool webrtc = false;
    bool portable = false;
    const char* preferred = "none";
};

inline AecCapabilities aec_capabilities() {
    AecCapabilities capabilities;
#if defined(PUCK_KIOSK_ENABLE_AEC)
    capabilities.webrtc = true;
#endif
#if defined(PUCK_KIOSK_ENABLE_PORTABLE_AEC)
    capabilities.portable = true;
#endif
    capabilities.built = capabilities.webrtc || capabilities.portable;
    capabilities.preferred = capabilities.webrtc ? "webrtc-audio-processing"
                              : capabilities.portable ? "speexdsp"
                                                      : "none";
    return capabilities;
}

enum class AecBackend {
    Auto,
    WebRtc,
    Portable,
};

struct AecSettings {
    // Off means a true bypass: no AEC, noise suppression, or gain control.
    // The app's On and Remote Only both set this true: every render frame the
    // engine supplies is the remote voice reference, and the kiosk has no
    // local playback source for Remote Only to exclude.
    bool echo_cancellation = true;
    bool noise_suppression = true;
    // AGC is deliberately off by default. In the file fixture SpeexDSP's
    // AGC lowered normal user speech by about 10 dB and cut its echo return
    // loss enhancement from 32.2 dB to 14.7 dB (quiet speech only gained
    // 2.0 dB). APM's AGC raised quiet speech by 10.4 dB but still cut ERLE
    // from 42.6 dB to 35.5 dB. The default keeps it off.
    bool automatic_gain = false;
    bool high_pass_filter = true;
    // Echo tail the portable adaptive filter covers. 200 ms covers the file
    // fixture's room and the WASAPI/ALSA device buffers; measure the target
    // cabin before increasing it, because more tail costs CPU.
    int tail_ms = 200;
    // Extra reference delay for a known device/room bulk delay. The playback
    // reference is normally the frame just written, and the adaptive filter
    // learns the acoustic delay itself; leave this at 0 unless a particular
    // installation has a bulk delay larger than tail_ms.
    int reference_delay_ms = 0;
    AecBackend backend = AecBackend::Auto;
};

// Settings the engine uses when it calls the two-argument initialize(). The
// Debug panel derives its effective NS/AGC report from this so the displayed
// processing cannot drift from the actual call path.
inline AecSettings default_aec_settings() {
    return AecSettings{};
}

struct AecStatus {
    bool initialized = false;
    bool echo_cancellation = false;
    bool noise_suppression = false;
    bool automatic_gain = false;
    std::string backend = "none";
    std::string detail;
    int sample_rate = 0;
    int frame_ms = 10;
    int tail_ms = 0;
};

// Echo cancellation, noise suppression, and optional gain control on the
// capture path. The render path must receive every frame that is played so the
// canceller has a reference signal.
//
// Linux prefers webrtc-audio-processing and falls back to the vendored
// SpeexDSP backend. Windows has no webrtc-audio-processing package, so it
// always uses SpeexDSP. Both backends process 10 ms blocks internally; callers
// should pass whole multiples of 10 ms (the engine passes 20 ms).
class EchoCanceller {
public:
    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    bool initialize(int sample_rate, int channels, std::string* error);
    bool initialize(int sample_rate, int channels, const AecSettings& settings,
                    std::string* error);
    bool enabled() const;
    const AecStatus& status() const;

    void process_render(const std::int16_t* samples, int frames);
    void process_capture(std::int16_t* samples, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Diagnostic used by --voice-aec-smoke. Feeds a deterministic speech-like
// render signal and its delayed, attenuated echo through the active backend
// and reports the echo return loss enhancement after convergence.
bool run_aec_smoke_test(int seconds, double* erle_db, std::string* error);
// Same check on one named backend; used by the file tests to exercise the
// portable path that ships on Windows.
bool run_aec_smoke_test(int seconds, AecBackend backend, double* erle_db,
                        std::string* error);

}  // namespace pk::voice
