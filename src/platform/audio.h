#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pk {

// Audio I/O is deliberately sample-oriented. The voice layer owns Opus/WebRTC;
// the platform only moves PCM between the device and the app.
class AudioIO {
public:
    virtual ~AudioIO() = default;

    virtual bool start_capture(int sample_rate, int channels, std::string* error) = 0;
    virtual void stop_capture() = 0;
    virtual std::size_t read_capture(std::int16_t* destination, std::size_t frames) = 0;

    virtual bool start_playback(int sample_rate, int channels, std::string* error) = 0;
    virtual void stop_playback() = 0;
    virtual std::size_t write_playback(const std::int16_t* source, std::size_t frames) = 0;
};

}  // namespace pk
