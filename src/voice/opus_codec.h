#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace pk::voice {

// 48 kHz mono Opus at 20 ms per frame, matching the browser client's voice
// track. The encoder owns its libopus state; the voice engine creates one per
// call and releases it when the call ends.
class OpusEncoder {
public:
    OpusEncoder();
    ~OpusEncoder();
    OpusEncoder(const OpusEncoder&) = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;

    bool initialize(int sample_rate, int channels, int bitrate, std::string* error);
    int encode(const std::int16_t* pcm, int frame_samples, std::vector<std::uint8_t>* output,
               std::string* error);

private:
    ::OpusEncoder* encoder_ = nullptr;
    int sample_rate_ = 48000;
    int channels_ = 1;
};

class OpusDecoder {
public:
    OpusDecoder();
    ~OpusDecoder();
    OpusDecoder(const OpusDecoder&) = delete;
    OpusDecoder& operator=(const OpusDecoder&) = delete;

    bool initialize(int sample_rate, int channels, std::string* error);
    int decode(const std::uint8_t* payload, std::size_t size, std::int16_t* pcm, int frame_samples,
               std::string* error);
    int decode_plc(std::int16_t* pcm, int frame_samples, std::string* error);

private:
    ::OpusDecoder* decoder_ = nullptr;
    int sample_rate_ = 48000;
    int channels_ = 1;
};

}  // namespace pk::voice
