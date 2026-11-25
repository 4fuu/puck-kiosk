#pragma once

#include "platform/audio.h"

#include <memory>
#include <string>

namespace pk {

// Default-endpoint preflight for the Windows backend. It is intentionally
// separate from creating the AudioIO: a machine without endpoints must still
// get a retryable WASAPI backend, not a permanent synthetic/file stand-in.
struct WasapiEndpointStatus {
    bool capture_available = false;
    bool playback_available = false;
    std::string capture_error;
    std::string playback_error;

    bool available() const { return capture_available && playback_available; }
    std::string error() const;
};

// Queries the current default capture and render endpoints. Each call is a
// fresh probe; no endpoint is cached.
WasapiEndpointStatus probe_wasapi_endpoints();

// Creates the WASAPI backend. Missing endpoints are not a creation failure:
// start_capture()/start_playback() re-fetch the default endpoints on every
// attempt, so a device that appears after launch can be used without
// recreating the platform. Returns nullptr only when COM cannot initialize.
std::unique_ptr<AudioIO> create_wasapi_audio(std::string* error);

}  // namespace pk
