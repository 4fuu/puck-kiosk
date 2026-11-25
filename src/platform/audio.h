#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pk {

// Audio I/O is deliberately sample-oriented. The voice layer owns Opus/WebRTC;
// the platform only moves PCM between the device and the app.
class AudioIO {
public:
    virtual ~AudioIO() = default;

    virtual const char* name() const = 0;

    virtual bool start_capture(int sample_rate, int channels, std::string* error) = 0;
    virtual void stop_capture() = 0;
    virtual std::size_t read_capture(std::int16_t* destination, std::size_t frames) = 0;

    virtual bool start_playback(int sample_rate, int channels, std::string* error) = 0;
    virtual void stop_playback() = 0;
    virtual std::size_t write_playback(const std::int16_t* source, std::size_t frames) = 0;
};

struct FileAudioOptions {
    int sample_rate = 48000;
    int channels = 1;
    double tone_hz = 440.0;
    double amplitude = 0.2;
    std::string input_path;
    std::string capture_path;
    std::string playback_path;
    bool realtime = true;
};

// Synthetic capture (a voiced harmonic tone) and a WAV playback sink. An empty
// path discards that direction, which makes this the null backend for headless
// voice tests. The platform factory uses it for headless auto, or when
// --audio file/null is explicit; a real desktop auto request and explicit
// hardware requests never fall back to it.
std::unique_ptr<AudioIO> create_file_audio(const FileAudioOptions& options, std::string* error);

struct AlsaAudioOptions {
    std::string capture_device = "default";
    std::string playback_device = "default";
};

// Returns nullptr with an error when the build or the machine has no ALSA.
std::unique_ptr<AudioIO> create_alsa_audio(const AlsaAudioOptions& options, std::string* error);

}  // namespace pk
