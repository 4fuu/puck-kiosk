#include "platform/win32/win32_audio.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define INITGUID
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pk {
namespace {

std::string hresult_message(const char* what, HRESULT result) {
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string text;
    if (buffer && length > 0) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (buffer) {
        LocalFree(buffer);
    }
    char code[16] = {};
    std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(result));
    return std::string(what) + " (" + code + ")" + (text.empty() ? "" : ": " + text);
}

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : pointer_(other.pointer_) { other.pointer_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    T** put() {
        reset();
        return &pointer_;
    }

    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

    void reset() {
        if (pointer_) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

// A bounded interleaved-sample queue between the WASAPI callback threads and
// the voice engine. Dropping the oldest samples keeps latency bounded when the
// consumer falls behind; silence is never inserted here.
class SampleRing {
public:
    void reset(std::size_t capacity_samples) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.assign(capacity_samples, 0);
        read_ = 0;
        size_ = 0;
    }

    void write(const std::int16_t* source, std::size_t count) {
        if (!source || count == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty()) {
            return;
        }
        if (count > samples_.size()) {
            source += count - samples_.size();
            count = samples_.size();
        }
        const std::size_t overflow = size_ + count > samples_.size() ? size_ + count - samples_.size() : 0;
        read_ = (read_ + overflow) % samples_.size();
        size_ -= overflow;
        for (std::size_t i = 0; i < count; ++i) {
            samples_[(read_ + size_) % samples_.size()] = source[i];
            ++size_;
        }
    }

    std::size_t read(std::int16_t* destination, std::size_t count) {
        if (!destination || count == 0) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty()) {
            return 0;
        }
        const std::size_t taken = std::min(count, size_);
        for (std::size_t i = 0; i < taken; ++i) {
            destination[i] = samples_[(read_ + i) % samples_.size()];
        }
        read_ = (read_ + taken) % samples_.size();
        size_ -= taken;
        return taken;
    }

private:
    std::mutex mutex_;
    std::vector<std::int16_t> samples_;
    std::size_t read_ = 0;
    std::size_t size_ = 0;
};

class WasapiAudioIO final : public AudioIO {
public:
    WasapiAudioIO() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized_ = SUCCEEDED(result);
        if (result == RPC_E_CHANGED_MODE) {
            log::warn("wasapi: COM is already initialized in a different apartment; continuing");
        }
    }

    ~WasapiAudioIO() override {
        stop_capture();
        stop_playback();
        if (com_initialized_) {
            CoUninitialize();
        }
    }

    const char* name() const override { return "wasapi"; }

    bool start_capture(int sample_rate, int channels, std::string* error) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (capture_running_) {
            return sample_rate == capture_sample_rate_ && channels == capture_channels_.load();
        }
        if (!open_client(true, sample_rate, channels, error)) {
            return false;
        }
        capture_ring_.reset(static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(channels));
        capture_running_ = true;
        try {
            capture_thread_ = std::thread(&WasapiAudioIO::capture_loop, this);
        } catch (const std::exception& exception) {
            capture_running_ = false;
            close_client(true);
            if (error) {
                *error = std::string("wasapi capture thread: ") + exception.what();
            }
            return false;
        }
        return true;
    }

    void stop_capture() override {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            if (!capture_running_ && !capture_thread_.joinable()) {
                close_client(true);
                return;
            }
            capture_running_ = false;
            if (capture_event_) {
                SetEvent(capture_event_);
            }
            thread = std::move(capture_thread_);
        }
        if (thread.joinable()) {
            thread.join();
        }
        std::lock_guard<std::mutex> lock(capture_mutex_);
        close_client(true);
    }

    std::size_t read_capture(std::int16_t* destination, std::size_t frames) override {
        const int channels = capture_channels_.load();
        if (!destination || frames == 0 || channels <= 0) {
            return 0;
        }
        return capture_ring_.read(destination, frames * static_cast<std::size_t>(channels)) /
               static_cast<std::size_t>(channels);
    }

    bool start_playback(int sample_rate, int channels, std::string* error) override {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (playback_running_) {
            return sample_rate == playback_sample_rate_ && channels == playback_channels_.load();
        }
        if (!open_client(false, sample_rate, channels, error)) {
            return false;
        }
        playback_ring_.reset(static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(channels));
        playback_running_ = true;
        try {
            playback_thread_ = std::thread(&WasapiAudioIO::playback_loop, this);
        } catch (const std::exception& exception) {
            playback_running_ = false;
            close_client(false);
            if (error) {
                *error = std::string("wasapi playback thread: ") + exception.what();
            }
            return false;
        }
        return true;
    }

    void stop_playback() override {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(playback_mutex_);
            if (!playback_running_ && !playback_thread_.joinable()) {
                close_client(false);
                return;
            }
            playback_running_ = false;
            if (playback_event_) {
                SetEvent(playback_event_);
            }
            thread = std::move(playback_thread_);
        }
        if (thread.joinable()) {
            thread.join();
        }
        std::lock_guard<std::mutex> lock(playback_mutex_);
        close_client(false);
    }

    std::size_t write_playback(const std::int16_t* source, std::size_t frames) override {
        const int channels = playback_channels_.load();
        if (!source || frames == 0 || channels <= 0) {
            return 0;
        }
        playback_ring_.write(source, frames * static_cast<std::size_t>(channels));
        return frames;
    }

private:
    bool open_client(bool capture, int sample_rate, int channels, std::string* error) {
        if (sample_rate <= 0 || channels <= 0) {
            if (error) {
                *error = "wasapi: invalid sample rate or channel count";
            }
            return false;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                          IID_IMMDeviceEnumerator,
                                          reinterpret_cast<void**>(enumerator.put()));
        if (FAILED(result)) {
            if (error) {
                *error = hresult_message("wasapi: cannot create the device enumerator", result);
            }
            return false;
        }
        ComPtr<IMMDevice> device;
        result = enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole,
                                                      device.put());
        if (FAILED(result)) {
            if (error) {
                *error = hresult_message(
                    capture ? "wasapi: no default capture device" : "wasapi: no default playback device",
                    result);
            }
            return false;
        }
        ComPtr<IAudioClient> client;
        result = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.put()));
        if (FAILED(result)) {
            if (error) {
                *error = hresult_message("wasapi: cannot activate the audio client", result);
            }
            return false;
        }

        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &format, nullptr);
        if (FAILED(result)) {
            if (error) {
                *error = hresult_message("wasapi: cannot initialize the audio client", result);
            }
            return false;
        }

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            if (error) {
                *error = "wasapi: cannot create the audio event";
            }
            return false;
        }
        result = client->SetEventHandle(event);
        if (FAILED(result)) {
            CloseHandle(event);
            if (error) {
                *error = hresult_message("wasapi: cannot set the audio event", result);
            }
            return false;
        }

        UINT32 buffer_frames = 0;
        result = client->GetBufferSize(&buffer_frames);
        if (FAILED(result)) {
            CloseHandle(event);
            if (error) {
                *error = hresult_message("wasapi: cannot read the buffer size", result);
            }
            return false;
        }

        if (capture) {
            ComPtr<IAudioCaptureClient> capture_client;
            result = client->GetService(IID_IAudioCaptureClient,
                                        reinterpret_cast<void**>(capture_client.put()));
            if (FAILED(result)) {
                CloseHandle(event);
                if (error) {
                    *error = hresult_message("wasapi: cannot get the capture client", result);
                }
                return false;
            }
            result = client->Start();
            if (FAILED(result)) {
                CloseHandle(event);
                if (error) {
                    *error = hresult_message("wasapi: cannot start capture", result);
                }
                return false;
            }
            capture_client_ = std::move(capture_client);
            capture_audio_client_ = std::move(client);
            capture_event_ = event;
            capture_buffer_frames_ = buffer_frames;
            capture_sample_rate_ = sample_rate;
            capture_channels_ = channels;
        } else {
            ComPtr<IAudioRenderClient> render_client;
            result = client->GetService(IID_IAudioRenderClient,
                                        reinterpret_cast<void**>(render_client.put()));
            if (FAILED(result)) {
                CloseHandle(event);
                if (error) {
                    *error = hresult_message("wasapi: cannot get the render client", result);
                }
                return false;
            }
            result = client->Start();
            if (FAILED(result)) {
                CloseHandle(event);
                if (error) {
                    *error = hresult_message("wasapi: cannot start playback", result);
                }
                return false;
            }
            render_client_ = std::move(render_client);
            render_audio_client_ = std::move(client);
            playback_event_ = event;
            playback_buffer_frames_ = buffer_frames;
            playback_sample_rate_ = sample_rate;
            playback_channels_ = channels;
        }
        log::info("wasapi: %s at %d Hz, %d channel(s)", capture ? "capture" : "playback",
                  sample_rate, channels);
        return true;
    }

    void close_client(bool capture) {
        if (capture) {
            if (capture_audio_client_) {
                capture_audio_client_->Stop();
            }
            capture_client_.reset();
            capture_audio_client_.reset();
            if (capture_event_) {
                CloseHandle(capture_event_);
                capture_event_ = nullptr;
            }
            capture_buffer_frames_ = 0;
        } else {
            if (render_audio_client_) {
                render_audio_client_->Stop();
            }
            render_client_.reset();
            render_audio_client_.reset();
            if (playback_event_) {
                CloseHandle(playback_event_);
                playback_event_ = nullptr;
            }
            playback_buffer_frames_ = 0;
        }
    }

    void capture_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::int16_t> silence;
        while (capture_running_) {
            const DWORD wait = WaitForSingleObject(capture_event_, 20);
            if (!capture_running_) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 packet = 0;
            while (capture_running_ && SUCCEEDED(capture_client_->GetNextPacketSize(&packet)) &&
                   packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    break;
                }
                if (frames > 0) {
                    const std::size_t samples =
                        static_cast<std::size_t>(frames) * static_cast<std::size_t>(capture_channels_.load());
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        silence.assign(samples, 0);
                        capture_ring_.write(silence.data(), samples);
                    } else if (data) {
                        capture_ring_.write(reinterpret_cast<const std::int16_t*>(data), samples);
                    }
                }
                capture_client_->ReleaseBuffer(frames);
            }
        }
        CoUninitialize();
    }

    void playback_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::int16_t> scratch;
        while (playback_running_) {
            const DWORD wait = WaitForSingleObject(playback_event_, 20);
            if (!playback_running_) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(render_audio_client_->GetCurrentPadding(&padding))) {
                continue;
            }
            if (padding >= playback_buffer_frames_) {
                continue;
            }
            const UINT32 available = playback_buffer_frames_ - padding;
            BYTE* data = nullptr;
            if (FAILED(render_client_->GetBuffer(available, &data))) {
                continue;
            }
            const int channels = playback_channels_.load();
            const std::size_t samples =
                static_cast<std::size_t>(available) * static_cast<std::size_t>(channels);
            scratch.assign(samples, 0);
            const std::size_t got = playback_ring_.read(scratch.data(), samples);
            if (got < samples) {
                std::fill(scratch.begin() + static_cast<std::ptrdiff_t>(got), scratch.end(), 0);
            }
            std::memcpy(data, scratch.data(), samples * sizeof(std::int16_t));
            render_client_->ReleaseBuffer(available, 0);
        }
        CoUninitialize();
    }

    bool com_initialized_ = false;

    std::mutex capture_mutex_;
    std::mutex playback_mutex_;
    std::thread capture_thread_;
    std::thread playback_thread_;
    std::atomic<bool> capture_running_{false};
    std::atomic<bool> playback_running_{false};
    std::atomic<int> capture_channels_{0};
    std::atomic<int> playback_channels_{0};
    int capture_sample_rate_ = 0;
    int playback_sample_rate_ = 0;

    ComPtr<IAudioClient> capture_audio_client_;
    ComPtr<IAudioClient> render_audio_client_;
    ComPtr<IAudioCaptureClient> capture_client_;
    ComPtr<IAudioRenderClient> render_client_;
    HANDLE capture_event_ = nullptr;
    HANDLE playback_event_ = nullptr;
    UINT32 capture_buffer_frames_ = 0;
    UINT32 playback_buffer_frames_ = 0;
    SampleRing capture_ring_;
    SampleRing playback_ring_;
};

}  // namespace

namespace {

bool probe_default_endpoint(bool capture, std::string* error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                      IID_IMMDeviceEnumerator,
                                      reinterpret_cast<void**>(enumerator.put()));
    if (FAILED(result)) {
        if (error) {
            *error = hresult_message("wasapi: cannot create the device enumerator", result);
        }
        return false;
    }
    ComPtr<IMMDevice> device;
    result = enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole,
                                                  device.put());
    if (FAILED(result)) {
        if (error) {
            *error = hresult_message(
                capture ? "wasapi: no default capture device" : "wasapi: no default playback device",
                result);
        }
        return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<AudioIO> create_wasapi_audio(std::string* error) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool initialized = SUCCEEDED(com);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        if (error) {
            *error = hresult_message("wasapi: COM initialization failed", com);
        }
        return nullptr;
    }

    // Both directions are required for a voice call. Probing here lets the
    // platform factory fall back to the file/null backend before the call
    // starts, instead of failing in the middle of one.
    const bool available = probe_default_endpoint(false, error) &&
                           probe_default_endpoint(true, error);
    if (initialized) {
        CoUninitialize();
    }
    if (!available) {
        return nullptr;
    }
    return std::make_unique<WasapiAudioIO>();
}

}  // namespace pk
