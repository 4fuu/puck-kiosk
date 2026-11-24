#pragma once

#include "platform/display.h"

#include <memory>
#include <string>

namespace pk {

// A Win32 window with a WGL OpenGL 3.3 core context. The window is created
// even in the hidden ("headless") mode because WGL needs a device context; a
// hidden window still renders and writes the PPM frame.
class Win32Display final : public Display {
public:
    Win32Display(int width, int height, std::string output_path, int frames, bool visible,
                 bool fullscreen);
    ~Win32Display() override;

    bool initialize(std::string* error) override;
    Size size() const override;
    float dpi_scale() const override;
    void make_current() override;
    void swap() override;
    bool should_close() const override;
    const char* backend_name() const override;

    // Drains the Win32 message queue. Input events are queued by the window
    // procedure and consumed with next_event().
    void pump_messages();
    bool next_event(InputEvent* event);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Win32Input final : public InputSource {
public:
    explicit Win32Input(Win32Display& display) : display_(display) {}
    bool poll(InputEvent& event) override;

private:
    Win32Display& display_;
};

}  // namespace pk
