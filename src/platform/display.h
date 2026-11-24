#pragma once

#include "core/types.h"

#include <string>

namespace pk {

struct InputEvent {
    enum class Type {
        None,
        PointerDown,
        PointerMove,
        PointerUp,
        KeyDown,
        KeyUp,
        GamepadButtonDown,
        GamepadButtonUp,
        GamepadAxis,
        Quit,
    };

    // The standard gamepad mapping shared by the evdev reader, XInput, and the
    // scripted-input hook. A is the south face button, B east, X north, Y west.
    enum class GamepadButton {
        A,
        B,
        X,
        Y,
        DPadUp,
        DPadDown,
        DPadLeft,
        DPadRight,
        Start,
        Back,
        LeftBumper,
        RightBumper,
    };

    // Sticks are normalized to [-1, 1], with up and left negative as in evdev.
    enum class GamepadAxis {
        LeftX,
        LeftY,
    };

    Type type = Type::None;
    float x = 0.0f;
    float y = 0.0f;
    int key = 0;
    GamepadButton button = GamepadButton::A;
    GamepadAxis axis = GamepadAxis::LeftX;
    float value = 0.0f;
};

inline bool is_gamepad_event(const InputEvent& event) {
    return event.type == InputEvent::Type::GamepadButtonDown ||
           event.type == InputEvent::Type::GamepadButtonUp ||
           event.type == InputEvent::Type::GamepadAxis;
}

// A drawable surface. Backends own the window/DRM/KMS/EGL details.
class Display {
public:
    virtual ~Display() = default;

    virtual bool initialize(std::string* error) = 0;
    virtual Size size() const = 0;
    virtual float dpi_scale() const = 0;
    virtual void make_current() = 0;
    virtual void swap() = 0;
    virtual bool should_close() const = 0;
    virtual const char* backend_name() const = 0;
};

// Touch, pointer, key, and gamepad input. Kept separate from Display so a
// libinput or evdev backend can be swapped in without touching the window
// backend.
class InputSource {
public:
    virtual ~InputSource() = default;
    virtual bool poll(InputEvent& event) = 0;
};

}  // namespace pk
