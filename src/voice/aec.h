#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace pk::voice {

// Echo cancellation, noise suppression, and a high-pass filter on the capture
// path. The render path must receive every frame that is played so the AEC has
// a reference signal. Frames may be any multiple of 10 ms; the implementation
// splits them for webrtc-audio-processing.
class EchoCanceller {
public:
    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    bool initialize(int sample_rate, int channels, std::string* error);
    bool enabled() const { return apm_ != nullptr; }

    void process_render(const std::int16_t* samples, int frames);
    void process_capture(std::int16_t* samples, int frames);

private:
    void* apm_ = nullptr;
    int sample_rate_ = 48000;
    int channels_ = 1;
    std::mutex mutex_;
};

// Diagnostic used by --voice-aec-smoke. Generates a delayed echo of a synthetic
// render signal and reports the echo return loss enhancement after convergence.
bool run_aec_smoke_test(int seconds, double* erle_db, std::string* error);

}  // namespace pk::voice
