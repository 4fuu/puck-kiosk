#pragma once

#include "platform/display.h"
#include "platform/win32/win32_display.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace pk {

// Polls XInput slots 0-3 on a background thread and emits the shared gamepad
// events. XInput is loaded dynamically so builds still run on Windows releases
// without xinput1_4.dll.
class XInputGamepad final : public InputSource {
public:
    XInputGamepad();
    ~XInputGamepad() override;

    bool poll(InputEvent& event) override;

private:
    void run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<InputEvent> events_;
    bool stop_ = false;
};

// Window messages first, then gamepad events, through one InputSource.
class Win32CombinedInput final : public InputSource {
public:
    explicit Win32CombinedInput(Win32Display& display) : window_(display) {}

    bool poll(InputEvent& event) override {
        if (window_.poll(event)) {
            return true;
        }
        return pad_.poll(event);
    }

private:
    Win32Input window_;
    XInputGamepad pad_;
};

}  // namespace pk
