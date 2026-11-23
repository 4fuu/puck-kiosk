#include "app/diagnostics.h"

#include "core/log.h"
#include "platform/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace pk {
namespace {

constexpr const char* kVersion = "0.1.0";
constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kMicTestFrames = 75;
constexpr int kMicTestFrameSamples = 480;

std::string number_string(double value, int decimals) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

}  // namespace

const char* echo_cancellation_value(EchoCancellationMode mode) {
    switch (mode) {
        case EchoCancellationMode::On: return "on";
        case EchoCancellationMode::RemoteOnly: return "remote-only";
        case EchoCancellationMode::Off: return "off";
    }
    return "on";
}

bool parse_echo_cancellation(const std::string& value, EchoCancellationMode* mode) {
    if (value == "on") {
        *mode = EchoCancellationMode::On;
        return true;
    }
    if (value == "remote-only" || value == "remote_only") {
        *mode = EchoCancellationMode::RemoteOnly;
        return true;
    }
    if (value == "off") {
        *mode = EchoCancellationMode::Off;
        return true;
    }
    return false;
}

Diagnostics::Diagnostics(HttpClient* http, std::string origin)
    : http_(http), origin_(std::move(origin)) {}

void Diagnostics::set_environment(Size viewport, float dpi, std::string backend, bool touch) {
    viewport_ = viewport;
    dpi_ = dpi > 0.0f ? dpi : 1.0f;
    backend_ = std::move(backend);
    touch_ = touch;
}

void Diagnostics::set_audio(std::string backend, bool available) {
    audio_backend_ = std::move(backend);
    audio_available_ = available;
}

void Diagnostics::set_session(bool can_send, std::string thread_id) {
    can_send_ = can_send;
    thread_id_ = std::move(thread_id);
}

std::string Diagnostics::user_agent() const {
    return "puck-kiosk/0.1 (" + backend_ + ")";
}

bool Diagnostics::run_mic_test(AudioIO* audio, std::string* error) {
    mic_ = MicTestResult{};
    if (call_active_) {
        mic_.error = "Microphone is busy in a call";
        if (error) {
            *error = mic_.error;
        }
        return false;
    }
    if (!audio) {
        mic_.error = "No microphone is available";
        if (error) {
            *error = mic_.error;
        }
        return false;
    }

    std::string start_error;
    if (!audio->start_capture(kSampleRate, kChannels, &start_error)) {
        mic_.error = start_error.empty() ? "Cannot open the microphone" : start_error;
        if (error) {
            *error = mic_.error;
        }
        return false;
    }

    std::vector<std::int16_t> samples(kMicTestFrameSamples);
    int peak = 0;
    int silent_reads = 0;
    for (int frame = 0; frame < kMicTestFrames; ++frame) {
        const std::size_t read = audio->read_capture(samples.data(), samples.size());
        if (read == 0) {
            ++silent_reads;
            if (silent_reads > 10) {
                break;
            }
            continue;
        }
        for (std::size_t i = 0; i < read; ++i) {
            peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        }
    }
    audio->stop_capture();

    mic_.ran = true;
    mic_.label = audio->name();
    mic_.ok = peak > 0;
    mic_.silent = peak == 0;
    mic_.peak_dbfs = peak > 0 ? 20.0 * std::log10(static_cast<double>(peak) / 32768.0) : -96.0;
    if (!mic_.ok) {
        mic_.error = "The microphone stayed silent";
        if (error) {
            *error = mic_.error;
        }
        return false;
    }
    log::info("diagnostics: mic test on %s, peak %.1f dBFS", mic_.label.c_str(), mic_.peak_dbfs);
    return true;
}

nlohmann::json Diagnostics::payload() const {
    nlohmann::json body;
    body["screen"] = "puck";
    body["userAgent"] = user_agent();
    body["clientHints"] = nullptr;
    body["innerWidth"] = viewport_.width;
    body["innerHeight"] = viewport_.height;
    body["screenWidth"] = viewport_.width;
    body["screenHeight"] = viewport_.height;
    body["devicePixelRatio"] = dpi_;
    body["maxTouchPoints"] = touch_ ? 1 : 0;
    body["pointer"] = touch_ ? "coarse" : "none";
    body["hover"] = false;
    body["webRTC"] = true;
    body["audioContextSampleRate"] =
        audio_available_ ? nlohmann::json(kSampleRate) : nlohmann::json(nullptr);
    body["microphonePermission"] = audio_available_ ? "granted" : "unavailable";
    body["echoCancellation"] = echo_cancellation_value(echo_);
    if (mic_.ran) {
        body["microphone"] = {
            {"result", mic_.ok ? "ok" : (mic_.silent ? "silent" : "error")},
            {"label", mic_.label},
            {"settings",
             {{"sampleRate", kSampleRate},
              {"channelCount", kChannels},
              {"echoCancellation", echo_ != EchoCancellationMode::Off},
              {"autoGainControl", true},
              {"noiseSuppression", true}}},
            {"peakDBFS", mic_.ok ? nlohmann::json(mic_.peak_dbfs) : nlohmann::json(nullptr)},
        };
    } else {
        body["microphone"] = nullptr;
    }
    body["devices"] = nlohmann::json::array();
    body["devices"].push_back(
        {{"kind", "audioinput"},
         {"label", audio_available_ ? audio_backend_ : std::string("(no audio backend)")}});
    return body;
}

bool Diagnostics::send(std::string* error) {
    if (!can_send_ || !http_) {
        send_status_ = "Sign in to send";
        if (error) {
            *error = "no car session";
        }
        return false;
    }
    HttpRequest request;
    request.method = "POST";
    request.url = origin_ + "/api/car/diagnostics";
    request.headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Origin", origin_},
        {"Referer", origin_ + "/car"},
        {"User-Agent", user_agent()},
    };
    request.body = payload().dump();

    std::string request_error;
    const HttpResponse response = http_->request(request, &request_error);
    if (!request_error.empty()) {
        send_status_ = "Failed (offline)";
        if (error) {
            *error = request_error;
        }
        log::warn("diagnostics: %s", request_error.c_str());
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        send_status_ = "Failed (" + std::to_string(response.status) + ")";
        if (error) {
            *error = "HTTP " + std::to_string(response.status);
        }
        log::warn("diagnostics: HTTP %d", response.status);
        return false;
    }
    send_status_ = "Sent";
    log::info("diagnostics: sent %zu bytes", request.body.size());
    return true;
}

DebugView Diagnostics::view() const {
    DebugView view;
    view.open = open_;
    view.echo = echo_;
    view.can_send = can_send_;
    view.mic_busy = call_active_;
    if (mic_.ran) {
        if (mic_.ok) {
            view.mic_status = "OK, peak " + number_string(mic_.peak_dbfs, 1) + " dBFS";
        } else {
            view.mic_status = mic_.silent ? "Silent" : mic_.error;
        }
    } else if (!mic_.error.empty()) {
        view.mic_status = mic_.error;
    }
    view.send_status = send_status_;

    view.rows = {
        {"Device", backend_ + " kiosk"},
        {"Firmware", std::string("puck-kiosk ") + kVersion},
        {"Renderer", "NanoVG / GLES3"},
        {"Chrome", "n/a (native)"},
        {"Auto Code", "n/a"},
        {"Viewport", std::to_string(viewport_.width) + "\u00d7" + std::to_string(viewport_.height) +
                         " @ " + number_string(dpi_, 1) + "x"},
        {"Screen", std::to_string(viewport_.width) + "\u00d7" + std::to_string(viewport_.height)},
        {"Touch", touch_ ? "1 point, pointer coarse, hover no" : "0 points, pointer none, hover no"},
        {"WebRTC", "Yes (libdatachannel)"},
        {"Audio Rate", audio_available_ ? std::to_string(kSampleRate) + " Hz" : "\u2014"},
        {"Mic Permission", audio_available_ ? "granted" : "unavailable"},
        {"Mic Test", view.mic_status},
        {"Mic Track", audio_available_ ? audio_backend_ : "\u2014"},
        {"Devices", audio_available_ ? "audioinput: " + audio_backend_ : "\u2014"},
        {"Client Hints", "n/a"},
        {"User Agent", user_agent()},
    };
    if (!thread_id_.empty()) {
        view.rows.push_back({"Thread", thread_id_});
    }
    if (mic_.ran && mic_.ok) {
        view.rows.push_back({"Mic Settings", "sampleRate: 48000, channelCount: 1, echoCancellation: " +
                                                 std::string(echo_ != EchoCancellationMode::Off ? "true" : "false")});
    }
    return view;
}

bool post_car_usage(HttpClient& http, const std::string& origin, const std::string& entry,
                    bool coarse_pointer, int viewport_width, int viewport_height,
                    std::string* error) {
    HttpRequest request;
    request.method = "POST";
    request.url = origin + "/api/car/usage";
    request.headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Origin", origin},
        {"Referer", origin + "/car"},
        {"User-Agent", "puck-kiosk/" + std::string(kVersion)},
    };
    request.body = nlohmann::json{
        {"entry", entry},
        {"coarsePointer", coarse_pointer},
        {"viewportWidth", viewport_width},
        {"viewportHeight", viewport_height},
    }.dump();

    std::string request_error;
    const HttpResponse response = http.request(request, &request_error);
    if (!request_error.empty()) {
        if (error) {
            *error = request_error;
        }
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        if (error) {
            *error = "HTTP " + std::to_string(response.status);
        }
        return false;
    }
    return true;
}

}  // namespace pk
