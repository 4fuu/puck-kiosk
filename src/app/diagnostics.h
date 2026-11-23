#pragma once

#include "core/types.h"
#include "platform/http.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pk {

class AudioIO;

enum class EchoCancellationMode {
    On,
    RemoteOnly,
    Off,
};

const char* echo_cancellation_value(EchoCancellationMode mode);
bool parse_echo_cancellation(const std::string& value, EchoCancellationMode* mode);

struct MicTestResult {
    bool ran = false;
    bool ok = false;
    bool silent = false;
    std::string label;
    double peak_dbfs = 0.0;
    std::string error;
};

struct DebugRow {
    std::string label;
    std::string value;
};

struct DebugView {
    bool open = false;
    EchoCancellationMode echo = EchoCancellationMode::On;
    std::vector<DebugRow> rows;
    std::string mic_status = "Not run";
    std::string send_status;
    bool can_send = false;
    bool mic_busy = false;
};

// The car page's Debug panel. It collects native device information, owns the
// echo-cancellation choice for the next call, runs a short microphone test, and
// posts the same JSON body the web sends to /api/car/diagnostics.
class Diagnostics {
public:
    Diagnostics(HttpClient* http, std::string origin);

    void set_environment(Size viewport, float dpi, std::string backend, bool touch);
    void set_audio(std::string backend, bool available);
    void set_session(bool can_send, std::string thread_id);
    void set_call_active(bool active) { call_active_ = active; }

    void open() { open_ = true; }
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    EchoCancellationMode echo_mode() const { return echo_; }
    void set_echo_mode(EchoCancellationMode mode) { echo_ = mode; }

    // Blocks for about 0.75 s while it samples the capture device.
    bool run_mic_test(AudioIO* audio, std::string* error);
    bool send(std::string* error);

    DebugView view() const;
    nlohmann::json payload() const;
    const std::string& send_status() const { return send_status_; }

private:
    std::string user_agent() const;

    HttpClient* http_ = nullptr;
    std::string origin_;
    Size viewport_;
    float dpi_ = 1.0f;
    std::string backend_ = "headless";
    bool touch_ = false;
    std::string audio_backend_ = "none";
    bool audio_available_ = false;
    bool can_send_ = false;
    bool call_active_ = false;
    bool open_ = false;
    std::string thread_id_;
    EchoCancellationMode echo_ = EchoCancellationMode::On;
    MicTestResult mic_;
    std::string send_status_;
};

// POST /api/car/usage with the same body the web sends when entering car mode.
bool post_car_usage(HttpClient& http, const std::string& origin, const std::string& entry,
                    bool coarse_pointer, int viewport_width, int viewport_height,
                    std::string* error);

}  // namespace pk
