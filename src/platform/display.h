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
        Quit,
    };

    Type type = Type::None;
    float x = 0.0f;
    float y = 0.0f;
    int key = 0;
};

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

// Touch, pointer, and key input. Kept separate from Display so a libinput or
// evdev backend can be swapped in without touching the window backend.
class InputSource {
public:
    virtual ~InputSource() = default;
    virtual bool poll(InputEvent& event) = 0;
};

}  // namespace pk
