#include "platform/win32/xinput_input.h"

#include "core/log.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pk {
namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn load_xinput() {
    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
        HMODULE module = LoadLibraryW(name);
        if (!module) {
            continue;
        }
        auto fn = reinterpret_cast<XInputGetStateFn>(
            reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
        if (fn) {
            return fn;
        }
        FreeLibrary(module);
    }
    return nullptr;
}

float stick(short value) {
    const float normalized = static_cast<float>(value) / 32768.0f;
    return std::clamp(normalized, -1.0f, 1.0f);
}

struct ButtonMap {
    WORD mask;
    InputEvent::GamepadButton button;
};

constexpr ButtonMap kButtons[] = {
    {XINPUT_GAMEPAD_A, InputEvent::GamepadButton::A},
    {XINPUT_GAMEPAD_B, InputEvent::GamepadButton::B},
    {XINPUT_GAMEPAD_X, InputEvent::GamepadButton::X},
    {XINPUT_GAMEPAD_Y, InputEvent::GamepadButton::Y},
    {XINPUT_GAMEPAD_DPAD_UP, InputEvent::GamepadButton::DPadUp},
    {XINPUT_GAMEPAD_DPAD_DOWN, InputEvent::GamepadButton::DPadDown},
    {XINPUT_GAMEPAD_DPAD_LEFT, InputEvent::GamepadButton::DPadLeft},
    {XINPUT_GAMEPAD_DPAD_RIGHT, InputEvent::GamepadButton::DPadRight},
    {XINPUT_GAMEPAD_START, InputEvent::GamepadButton::Start},
    {XINPUT_GAMEPAD_BACK, InputEvent::GamepadButton::Back},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, InputEvent::GamepadButton::LeftBumper},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, InputEvent::GamepadButton::RightBumper},
};

}  // namespace

XInputGamepad::XInputGamepad() : thread_([this] { run(); }) {}

XInputGamepad::~XInputGamepad() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool XInputGamepad::poll(InputEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
        return false;
    }
    event = events_.front();
    events_.pop_front();
    return true;
}

void XInputGamepad::run() {
    const XInputGetStateFn get_state = load_xinput();
    if (!get_state) {
        log::info("xinput: no XInput DLL; gamepad support disabled");
        return;
    }
    WORD previous[4] = {};
    float axes[4][2] = {};
    bool connected[4] = {};
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(33), [this] { return stop_; });
            if (stop_) {
                return;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (DWORD slot = 0; slot < 4; ++slot) {
            XINPUT_STATE state{};
            const bool present = get_state(slot, &state) == ERROR_SUCCESS;
            if (present != connected[slot]) {
                connected[slot] = present;
                log::info("xinput: slot %lu %s", static_cast<unsigned long>(slot),
                          present ? "connected" : "disconnected");
                if (!present) {
                    previous[slot] = 0;
                    continue;
                }
            }
            if (!present) {
                continue;
            }
            const WORD buttons = state.Gamepad.wButtons;
            for (const ButtonMap& mapping : kButtons) {
                const bool was = (previous[slot] & mapping.mask) != 0;
                const bool now = (buttons & mapping.mask) != 0;
                if (was == now) {
                    continue;
                }
                InputEvent event;
                event.type = now ? InputEvent::Type::GamepadButtonDown
                                 : InputEvent::Type::GamepadButtonUp;
                event.button = mapping.button;
                events_.push_back(event);
            }
            previous[slot] = buttons;
            const float lx = stick(state.Gamepad.sThumbLX);
            const float ly = -stick(state.Gamepad.sThumbLY);
            if (std::fabs(lx - axes[slot][0]) > 0.02f || std::fabs(ly - axes[slot][1]) > 0.02f) {
                axes[slot][0] = lx;
                axes[slot][1] = ly;
                InputEvent x;
                x.type = InputEvent::Type::GamepadAxis;
                x.axis = InputEvent::GamepadAxis::LeftX;
                x.value = lx;
                events_.push_back(x);
                InputEvent y;
                y.type = InputEvent::Type::GamepadAxis;
                y.axis = InputEvent::GamepadAxis::LeftY;
                y.value = ly;
                events_.push_back(y);
            }
        }
    }
}

}  // namespace pk
