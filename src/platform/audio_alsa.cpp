#include "platform/audio.h"

#include <mutex>
#include <string>

#if PUCK_KIOSK_ENABLE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace pk {
namespace {

#if PUCK_KIOSK_ENABLE_ALSA

std::string alsa_error(const char* what, int code) {
    return std::string(what) + ": " + snd_strerror(code);
}

class AlsaAudioIO final : public AudioIO {
public:
    explicit AlsaAudioIO(AlsaAudioOptions options) : options_(std::move(options)) {}

    ~AlsaAudioIO() override {
        stop_capture();
        stop_playback();
    }

    const char* name() const override { return "alsa"; }

    bool start_capture(int sample_rate, int channels, std::string* error) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (capture_) {
            return true;
        }
        int code = snd_pcm_open(&capture_, options_.capture_device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        if (code < 0) {
            if (error) {
                *error = alsa_error("cannot open capture device", code);
            }
            capture_ = nullptr;
            return false;
        }
        code = snd_pcm_set_params(capture_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  static_cast<unsigned int>(channels),
                                  static_cast<unsigned int>(sample_rate), 1, 40000);
        if (code < 0) {
            if (error) {
                *error = alsa_error("cannot configure capture device", code);
            }
            snd_pcm_close(capture_);
            capture_ = nullptr;
            return false;
        }
        snd_pcm_prepare(capture_);
        return true;
    }

    void stop_capture() override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!capture_) {
            return;
        }
        snd_pcm_drop(capture_);
        snd_pcm_close(capture_);
        capture_ = nullptr;
    }

    std::size_t read_capture(std::int16_t* destination, std::size_t frames) override {
        if (!destination || frames == 0) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!capture_) {
            return 0;
        }
        const int ready = snd_pcm_wait(capture_, 100);
        if (ready < 0) {
            snd_pcm_recover(capture_, ready, 1);
            return 0;
        }
        if (ready == 0) {
            return 0;
        }
        const snd_pcm_sframes_t got = snd_pcm_readi(capture_, destination, frames);
        if (got == -EPIPE) {
            snd_pcm_prepare(capture_);
            return 0;
        }
        if (got < 0) {
            snd_pcm_recover(capture_, static_cast<int>(got), 1);
            return 0;
        }
        return static_cast<std::size_t>(got);
    }

    bool start_playback(int sample_rate, int channels, std::string* error) override {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (playback_) {
            return true;
        }
        int code = snd_pcm_open(&playback_, options_.playback_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (code < 0) {
            if (error) {
                *error = alsa_error("cannot open playback device", code);
            }
            playback_ = nullptr;
            return false;
        }
        code = snd_pcm_set_params(playback_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  static_cast<unsigned int>(channels),
                                  static_cast<unsigned int>(sample_rate), 1, 40000);
        if (code < 0) {
            if (error) {
                *error = alsa_error("cannot configure playback device", code);
            }
            snd_pcm_close(playback_);
            playback_ = nullptr;
            return false;
        }
        snd_pcm_prepare(playback_);
        return true;
    }

    void stop_playback() override {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (!playback_) {
            return;
        }
        snd_pcm_drop(playback_);
        snd_pcm_close(playback_);
        playback_ = nullptr;
    }

    std::size_t write_playback(const std::int16_t* source, std::size_t frames) override {
        if (!source || frames == 0) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (!playback_) {
            return 0;
        }
        snd_pcm_sframes_t written = snd_pcm_writei(playback_, source, frames);
        if (written == -EPIPE) {
            snd_pcm_prepare(playback_);
            written = snd_pcm_writei(playback_, source, frames);
        } else if (written < 0) {
            snd_pcm_recover(playback_, static_cast<int>(written), 1);
            written = 0;
        }
        return written > 0 ? static_cast<std::size_t>(written) : 0;
    }

private:
    AlsaAudioOptions options_;
    std::mutex capture_mutex_;
    std::mutex playback_mutex_;
    snd_pcm_t* capture_ = nullptr;
    snd_pcm_t* playback_ = nullptr;
};

#endif  // PUCK_KIOSK_ENABLE_ALSA

}  // namespace

std::unique_ptr<AudioIO> create_alsa_audio(const AlsaAudioOptions& options, std::string* error) {
#if PUCK_KIOSK_ENABLE_ALSA
    return std::make_unique<AlsaAudioIO>(options);
#else
    (void)options;
    if (error) {
        *error = "built without ALSA (install libasound2-dev and reconfigure)";
    }
    return nullptr;
#endif
}

}  // namespace pk
