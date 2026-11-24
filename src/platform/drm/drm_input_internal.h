#pragma once

#include "platform/display.h"

#include <linux/input.h>

#include <algorithm>
#include <memory>
#include <string>

namespace pk {

std::unique_ptr<InputSource> create_libinput_input(const Display& display, std::string* error);
std::unique_ptr<InputSource> create_evdev_input(const Display& display, std::string* error);
std::unique_ptr<InputSource> create_evdev_gamepad_input(const Display& display,
                                                        std::string* error);

namespace drm_input {

inline bool set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

inline bool is_quit_key(int code) {
    return code == KEY_ESC;
}

inline bool is_button_code(int code) {
    return code >= BTN_MISC && code < BTN_MISC + 0x60;
}

// Maps a raw absolute axis value onto [0, extent - 1] pixels.
inline float map_axis(int value, int minimum, int maximum, int extent) {
    if (extent <= 0 || maximum <= minimum) {
        return 0.0f;
    }
    const float scaled = static_cast<float>(value - minimum) / static_cast<float>(maximum - minimum);
    const float limit = static_cast<float>(extent - 1);
    return std::clamp(scaled * limit, 0.0f, limit);
}

inline float clamp_to(float value, int extent) {
    if (extent <= 0) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, static_cast<float>(extent - 1));
}

}  // namespace drm_input

}  // namespace pk
