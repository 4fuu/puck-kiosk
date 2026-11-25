#pragma once

#include "core/types.h"
#include "platform/http.h"
#include "voice/aec.h"

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

// Configuration for the next call, plus the backends compiled into this
// binary. A request is not proof that a backend initialized, so this is kept
// separate from the current call's actual status below.
struct EchoCancellationState {
    bool supported = false;
    bool requested = false;
    bool effective = false;
    bool noise_suppression = false;
    bool automatic_gain = false;
    std::string backend = "none";
    std::string detail;
};

EchoCancellationState echo_cancellation_state(EchoCancellationMode mode);

// Actual state reported by the EchoCanceller that is running the current
// call. The call owner sets this after start_audio(); until then `known` is
// false and the UI says "not reported" rather than claiming a mode is active.
struct AecCallStatus {
    bool known = false;
    bool in_call = false;
    bool active = false;
    bool echo_cancellation = false;
    bool noise_suppression = false;
    bool automatic_gain = false;
    std::string backend = "none";
    std::string detail = "not reported";
};

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
    bool aec_supported = false;
    bool aec_effective = false;
    bool aec_noise_suppression = false;
    bool aec_automatic_gain = false;
    std::string aec_backend = "none";
    bool aec_current_known = false;
    bool aec_current_in_call = false;
    bool aec_current_active = false;
    bool aec_current_noise_suppression = false;
    bool aec_current_automatic_gain = false;
    std::string aec_current_backend = "none";
    std::string aec_current_detail;
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

    // The user's selection, as shown by the Debug radio and posted to the web.
    EchoCancellationMode requested_echo_mode() const { return echo_; }
    void set_echo_mode(EchoCancellationMode mode) { echo_ = mode; }

    // The mode the voice engine must use. On and Remote Only both request
    // echo cancellation: the kiosk has no local playback source, so every
    // render frame is the remote voice reference. Off, or a build with no AEC
    // backend, returns Off. Use requested_echo_mode() for display.
    EchoCancellationMode echo_mode() const;

    // The one value the call setup needs. Equivalent to echo_mode() == On.
    bool aec_enabled_for_call() const;
    EchoCancellationState echo_state() const;

    // The call owner reports the engine's actual EchoCanceller status here.
    // set_call_aec_status(false, status) records that a call is not running;
    // clear_call_aec_status() means no report is available.
    void set_call_aec_status(bool in_call, const voice::AecStatus& status);
    void clear_call_aec_status();
    AecCallStatus call_aec_status() const;

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
    AecCallStatus call_aec_;
    MicTestResult mic_;
    std::string send_status_;
};

// POST /api/car/usage with the same body the web sends when entering car mode.
bool post_car_usage(HttpClient& http, const std::string& origin, const std::string& entry,
                    bool coarse_pointer, int viewport_width, int viewport_height,
                    std::string* error);

}  // namespace pk
