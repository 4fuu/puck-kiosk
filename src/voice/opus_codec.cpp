#include "voice/opus_codec.h"

#include <opus.h>

namespace pk::voice {
namespace {

std::string opus_error(int code) {
    return std::string("libopus: ") + opus_strerror(code);
}

}  // namespace

OpusEncoder::OpusEncoder() = default;

OpusEncoder::~OpusEncoder() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
    }
}

bool OpusEncoder::initialize(int sample_rate, int channels, int bitrate, std::string* error) {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    if (sample_rate != 48000 || channels != 1) {
        if (error) {
            *error = "Opus voice is fixed at 48 kHz mono";
        }
        return false;
    }

    int status = OPUS_OK;
    encoder_ = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &status);
    if (!encoder_ || status != OPUS_OK) {
        if (error) {
            *error = opus_error(status);
        }
        encoder_ = nullptr;
        return false;
    }
    sample_rate_ = sample_rate;
    channels_ = channels;

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
    return true;
}

int OpusEncoder::encode(const std::int16_t* pcm, int frame_samples,
                        std::vector<std::uint8_t>* output, std::string* error) {
    if (!encoder_ || !pcm || !output) {
        if (error) {
            *error = "Opus encoder is not initialized";
        }
        return -1;
    }
    const int max_size = 4000;
    output->resize(static_cast<std::size_t>(max_size));
    const int size = opus_encode(encoder_, pcm, frame_samples, output->data(), max_size);
    if (size < 0) {
        if (error) {
            *error = opus_error(size);
        }
        output->clear();
        return -1;
    }
    output->resize(static_cast<std::size_t>(size));
    return size;
}

OpusDecoder::OpusDecoder() = default;

OpusDecoder::~OpusDecoder() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
    }
}

bool OpusDecoder::initialize(int sample_rate, int channels, std::string* error) {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    if (sample_rate != 48000 || channels != 1) {
        if (error) {
            *error = "Opus voice is fixed at 48 kHz mono";
        }
        return false;
    }

    int status = OPUS_OK;
    decoder_ = opus_decoder_create(sample_rate, channels, &status);
    if (!decoder_ || status != OPUS_OK) {
        if (error) {
            *error = opus_error(status);
        }
        decoder_ = nullptr;
        return false;
    }
    sample_rate_ = sample_rate;
    channels_ = channels;
    return true;
}

int OpusDecoder::decode(const std::uint8_t* payload, std::size_t size, std::int16_t* pcm,
                        int frame_samples, std::string* error) {
    if (!decoder_ || !pcm) {
        if (error) {
            *error = "Opus decoder is not initialized";
        }
        return -1;
    }
    const int decoded = opus_decode(decoder_, payload, static_cast<opus_int32>(size), pcm,
                                    frame_samples, 0);
    if (decoded < 0) {
        if (error) {
            *error = opus_error(decoded);
        }
        return -1;
    }
    return decoded;
}

int OpusDecoder::decode_plc(std::int16_t* pcm, int frame_samples, std::string* error) {
    if (!decoder_ || !pcm) {
        if (error) {
            *error = "Opus decoder is not initialized";
        }
        return -1;
    }
    const int decoded = opus_decode(decoder_, nullptr, 0, pcm, frame_samples, 0);
    if (decoded < 0) {
        if (error) {
            *error = opus_error(decoded);
        }
        return -1;
    }
    return decoded;
}

}  // namespace pk::voice
