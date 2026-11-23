#include "platform/audio.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace pk {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool read_wav_mono(const std::string& path, int expected_rate, std::vector<std::int16_t>* samples,
                   std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot open " + path;
        return false;
    }
    const auto read_u32 = [&in]() {
        unsigned char b[4] = {};
        in.read(reinterpret_cast<char*>(b), 4);
        return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
               (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    };
    const auto read_u16 = [&in]() {
        unsigned char b[2] = {};
        in.read(reinterpret_cast<char*>(b), 2);
        return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    };
    char riff[4] = {};
    in.read(riff, 4);
    read_u32();
    char wave[4] = {};
    in.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
        *error = path + " is not a RIFF/WAVE file";
        return false;
    }
    bool have_format = false;
    while (in) {
        char id[4] = {};
        in.read(id, 4);
        const std::uint32_t size = read_u32();
        const std::string chunk(id, 4);
        if (chunk == "fmt ") {
            const std::uint16_t format = read_u16();
            const std::uint16_t channels = read_u16();
            const std::uint32_t rate = read_u32();
            read_u32();
            read_u16();
            const std::uint16_t bits = read_u16();
            if (format != 1 || channels != 1 || bits != 16 ||
                rate != static_cast<std::uint32_t>(expected_rate)) {
                *error = path + " must be mono 16-bit PCM at " + std::to_string(expected_rate) + " Hz";
                return false;
            }
            have_format = true;
            if (size > 16) {
                in.seekg(size - 16, std::ios::cur);
            }
        } else if (chunk == "data") {
            if (!have_format) {
                *error = path + " has data before fmt";
                return false;
            }
            samples->resize(size / 2);
            in.read(reinterpret_cast<char*>(samples->data()), size);
            if (static_cast<std::uint32_t>(in.gcount()) != size) {
                *error = path + " is truncated";
                return false;
            }
            return !samples->empty();
        } else {
            in.seekg(size + (size & 1u), std::ios::cur);
        }
    }
    *error = path + " has no data chunk";
    return false;
}

void write_u16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
    out.write(bytes, 2);
}

void write_u32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    out.write(bytes, 4);
}

class WavWriter {
public:
    ~WavWriter() {
        std::string ignored;
        close(&ignored);
    }

    bool open(const std::string& path, int sample_rate, int channels, std::string* error) {
        close(nullptr);
        if (path.empty()) {
            return true;
        }
        const std::filesystem::path output(path);
        if (output.has_parent_path()) {
            std::error_code code;
            std::filesystem::create_directories(output.parent_path(), code);
        }
        file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file_) {
            if (error) {
                *error = "cannot open " + path;
            }
            return false;
        }
        path_ = path;
        sample_rate_ = sample_rate;
        channels_ = channels;
        data_bytes_ = 0;
        file_.write("RIFF", 4);
        write_u32(file_, 36);
        file_.write("WAVE", 4);
        file_.write("fmt ", 4);
        write_u32(file_, 16);
        write_u16(file_, 1);
        write_u16(file_, static_cast<std::uint16_t>(channels));
        write_u32(file_, static_cast<std::uint32_t>(sample_rate));
        write_u32(file_, static_cast<std::uint32_t>(sample_rate * channels * 2));
        write_u16(file_, static_cast<std::uint16_t>(channels * 2));
        write_u16(file_, 16);
        file_.write("data", 4);
        write_u32(file_, 0);
        return true;
    }

    void write(const std::int16_t* samples, std::size_t frames) {
        if (!file_.is_open() || !samples) {
            return;
        }
        const std::size_t bytes = frames * static_cast<std::size_t>(channels_) * sizeof(std::int16_t);
        file_.write(reinterpret_cast<const char*>(samples), static_cast<std::streamsize>(bytes));
        data_bytes_ += bytes;
    }

    bool close(std::string* error) {
        if (!file_.is_open()) {
            return true;
        }
        const std::uint64_t clamped = std::min<std::uint64_t>(data_bytes_, 0xffffffffull - 36);
        file_.seekp(4);
        write_u32(file_, static_cast<std::uint32_t>(36 + clamped));
        file_.seekp(40);
        write_u32(file_, static_cast<std::uint32_t>(clamped));
        file_.flush();
        const bool ok = file_.good();
        file_.close();
        if (!ok && error) {
            *error = "cannot finalize " + path_;
        }
        path_.clear();
        data_bytes_ = 0;
        return ok;
    }

    std::uint64_t data_bytes() const { return data_bytes_; }

private:
    std::ofstream file_;
    std::string path_;
    int sample_rate_ = 48000;
    int channels_ = 1;
    std::uint64_t data_bytes_ = 0;
};

class FileAudioIO final : public AudioIO {
public:
    explicit FileAudioIO(FileAudioOptions options) : options_(std::move(options)) {}

    const char* name() const override { return "file"; }

    bool start_capture(int sample_rate, int channels, std::string* error) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (sample_rate <= 0 || channels <= 0) {
            if (error) {
                *error = "invalid capture format";
            }
            return false;
        }
        capture_rate_ = sample_rate;
        capture_channels_ = channels;
        capture_phase_ = 0.0;
        capture_time_ = 0.0;
        capture_next_ = std::chrono::steady_clock::now();
        capture_running_ = false;
        capture_input_.clear();
        input_pos_ = 0;
        if (!options_.input_path.empty() &&
            !read_wav_mono(options_.input_path, sample_rate, &capture_input_, error)) {
            return false;
        }
        if (!capture_wav_.open(options_.capture_path, sample_rate, channels, error)) {
            return false;
        }
        capture_running_ = true;
        return true;
    }

    void stop_capture() override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        capture_running_ = false;
        std::string ignored;
        capture_wav_.close(&ignored);
    }

    std::size_t read_capture(std::int16_t* destination, std::size_t frames) override {
        if (!destination || frames == 0) {
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            if (!capture_running_) {
                return 0;
            }
            std::vector<std::int16_t> interleaved(frames * static_cast<std::size_t>(capture_channels_));
            for (std::size_t frame = 0; frame < frames; ++frame) {
                std::int16_t sample = 0;
                if (!capture_input_.empty()) {
                    sample = capture_input_[input_pos_ % capture_input_.size()];
                    ++input_pos_;
                } else {
                    const double envelope = 0.55 + 0.45 * std::sin(2.0 * kPi * 2.2 * capture_time_);
                    double value = std::sin(capture_phase_) + 0.35 * std::sin(2.0 * capture_phase_) +
                                   0.18 * std::sin(3.0 * capture_phase_);
                    value *= options_.amplitude * envelope / 1.53;
                    sample = static_cast<std::int16_t>(
                        std::clamp(value * 32767.0, -32768.0, 32767.0));
                    capture_phase_ += 2.0 * kPi * options_.tone_hz / capture_rate_;
                    if (capture_phase_ > 2.0 * kPi) {
                        capture_phase_ -= 2.0 * kPi;
                    }
                }
                for (int channel = 0; channel < capture_channels_; ++channel) {
                    interleaved[frame * static_cast<std::size_t>(capture_channels_) +
                                static_cast<std::size_t>(channel)] = sample;
                }
                capture_time_ += 1.0 / capture_rate_;
            }
            capture_wav_.write(interleaved.data(), frames);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                destination[frame] = interleaved[frame * static_cast<std::size_t>(capture_channels_)];
            }
        }
        if (options_.realtime) {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            const auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(frames) / capture_rate_));
            capture_next_ += duration;
            const auto now = std::chrono::steady_clock::now();
            if (capture_next_ > now) {
                std::this_thread::sleep_until(capture_next_);
            } else if (now - capture_next_ > std::chrono::milliseconds(200)) {
                capture_next_ = now;
            }
        }
        return frames;
    }

    bool start_playback(int sample_rate, int channels, std::string* error) override {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (sample_rate <= 0 || channels <= 0) {
            if (error) {
                *error = "invalid playback format";
            }
            return false;
        }
        if (!playback_wav_.open(options_.playback_path, sample_rate, channels, error)) {
            return false;
        }
        playback_running_ = true;
        return true;
    }

    void stop_playback() override {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        playback_running_ = false;
        std::string ignored;
        playback_wav_.close(&ignored);
    }

    std::size_t write_playback(const std::int16_t* source, std::size_t frames) override {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (!playback_running_ || !source) {
            return 0;
        }
        playback_wav_.write(source, frames);
        return frames;
    }

private:
    FileAudioOptions options_;
    std::mutex capture_mutex_;
    std::mutex playback_mutex_;
    WavWriter capture_wav_;
    WavWriter playback_wav_;
    bool capture_running_ = false;
    bool playback_running_ = false;
    int capture_rate_ = 48000;
    int capture_channels_ = 1;
    double capture_phase_ = 0.0;
    double capture_time_ = 0.0;
    std::vector<std::int16_t> capture_input_;
    std::size_t input_pos_ = 0;
    std::chrono::steady_clock::time_point capture_next_;
};

}  // namespace

std::unique_ptr<AudioIO> create_file_audio(const FileAudioOptions& options, std::string* error) {
    if (options.sample_rate <= 0 || options.channels <= 0 || options.channels > 2) {
        if (error) {
            *error = "file audio requires a positive sample rate and one or two channels";
        }
        return nullptr;
    }
    return std::make_unique<FileAudioIO>(options);
}

}  // namespace pk
