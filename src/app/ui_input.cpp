#include "app/ui_input.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pk {
namespace {

// evdev KEY_ESC is 1; X11 and ASCII use 27. Both backends feed InputEvent::key
// unchanged, so accept either.
bool is_escape_key(int key) {
    return key == 1 || key == 27;
}

bool parse_event_type(const std::string& type, InputEvent::Type* out) {
    if (type == "down" || type == "pointerdown" || type == "pointer_down") {
        *out = InputEvent::Type::PointerDown;
    } else if (type == "up" || type == "pointerup" || type == "pointer_up") {
        *out = InputEvent::Type::PointerUp;
    } else if (type == "move" || type == "pointermove" || type == "pointer_move") {
        *out = InputEvent::Type::PointerMove;
    } else if (type == "key" || type == "keydown" || type == "key_down") {
        *out = InputEvent::Type::KeyDown;
    } else if (type == "keyup" || type == "key_up") {
        *out = InputEvent::Type::KeyUp;
    } else if (type == "quit") {
        *out = InputEvent::Type::Quit;
    } else {
        return false;
    }
    return true;
}

bool json_number(const nlohmann::json& value, double* out) {
    if (value.is_number()) {
        *out = value.get<double>();
        return true;
    }
    return false;
}

bool parse_gamepad_button(const std::string& name, InputEvent::GamepadButton* out) {
    if (name == "a") {
        *out = InputEvent::GamepadButton::A;
    } else if (name == "b") {
        *out = InputEvent::GamepadButton::B;
    } else if (name == "x") {
        *out = InputEvent::GamepadButton::X;
    } else if (name == "y") {
        *out = InputEvent::GamepadButton::Y;
    } else if (name == "dpad_up") {
        *out = InputEvent::GamepadButton::DPadUp;
    } else if (name == "dpad_down") {
        *out = InputEvent::GamepadButton::DPadDown;
    } else if (name == "dpad_left") {
        *out = InputEvent::GamepadButton::DPadLeft;
    } else if (name == "dpad_right") {
        *out = InputEvent::GamepadButton::DPadRight;
    } else if (name == "start") {
        *out = InputEvent::GamepadButton::Start;
    } else if (name == "back") {
        *out = InputEvent::GamepadButton::Back;
    } else if (name == "lb" || name == "left_bumper") {
        *out = InputEvent::GamepadButton::LeftBumper;
    } else if (name == "rb" || name == "right_bumper") {
        *out = InputEvent::GamepadButton::RightBumper;
    } else {
        return false;
    }
    return true;
}

bool parse_gamepad_axis(const std::string& name, InputEvent::GamepadAxis* out) {
    if (name == "left_x" || name == "lx") {
        *out = InputEvent::GamepadAxis::LeftX;
    } else if (name == "left_y" || name == "ly") {
        *out = InputEvent::GamepadAxis::LeftY;
    } else {
        return false;
    }
    return true;
}

bool parse_gamepad_state(const std::string& state, bool* down) {
    if (state == "down" || state == "press" || state == "pressed") {
        *down = true;
    } else if (state == "up" || state == "release" || state == "released") {
        *down = false;
    } else {
        return false;
    }
    return true;
}

}  // namespace

const char* ui_action_name(UiAction action) {
    switch (action) {
        case UiAction::None: return "none";
        case UiAction::Call: return "call";
        case UiAction::Mute: return "mute";
        case UiAction::End: return "end";
        case UiAction::MediaClose: return "media-close";
        case UiAction::MediaPrevious: return "media-previous";
        case UiAction::MediaNext: return "media-next";
        case UiAction::SheetOption: return "sheet-option";
        case UiAction::SheetDismiss: return "sheet-dismiss";
        case UiAction::SheetOk: return "sheet-ok";
        case UiAction::PasskeyConfirm: return "passkey-confirm";
        case UiAction::DebugOpen: return "debug-open";
        case UiAction::DebugClose: return "debug-close";
        case UiAction::EchoMode: return "echo-mode";
        case UiAction::MicTest: return "mic-test";
        case UiAction::SendDiagnostics: return "send-diagnostics";
        case UiAction::Escape: return "escape";
    }
    return "none";
}

bool rect_contains(Rect rect, float x, float y) {
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
}

void UiHitMap::add(Rect rect, UiAction action, int index) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    targets_.push_back(UiTarget{rect, action, index});
}

const UiTarget* UiHitMap::hit(float x, float y) const {
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
        if (rect_contains(it->rect, x, y)) {
            return &*it;
        }
    }
    return nullptr;
}

void UiInputRouter::reset() {
    armed_ = false;
    pressed_ = UiTarget{};
    quit_ = false;
}

UiAction UiInputRouter::handle(const InputEvent& event, int* index) {
    if (index) {
        *index = -1;
    }
    switch (event.type) {
        case InputEvent::Type::Quit:
            quit_ = true;
            return UiAction::None;
        case InputEvent::Type::KeyDown:
            if (is_escape_key(event.key)) {
                armed_ = false;
                return UiAction::Escape;
            }
            return UiAction::None;
        case InputEvent::Type::PointerDown: {
            armed_ = false;
            if (!map_) {
                return UiAction::None;
            }
            const UiTarget* target = map_->hit(event.x, event.y);
            if (!target) {
                return UiAction::None;
            }
            pressed_ = *target;
            armed_ = true;
            return UiAction::None;
        }
        case InputEvent::Type::PointerUp: {
            if (!armed_) {
                return UiAction::None;
            }
            armed_ = false;
            if (!rect_contains(pressed_.rect, event.x, event.y)) {
                return UiAction::None;
            }
            if (index) {
                *index = pressed_.index;
            }
            return pressed_.action;
        }
        case InputEvent::Type::PointerMove:
        case InputEvent::Type::KeyUp:
        case InputEvent::Type::GamepadButtonDown:
        case InputEvent::Type::GamepadButtonUp:
        case InputEvent::Type::GamepadAxis:
        case InputEvent::Type::None:
            return UiAction::None;
    }
    return UiAction::None;
}

bool parse_input_script(const std::string& text, std::vector<ScriptedInput>* events,
                        std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!events) {
        return fail("no output for the input script");
    }
    events->clear();

    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return fail("input script is not valid JSON");
    }
    if (!parsed.is_array()) {
        return fail("input script must be a JSON array");
    }
    if (parsed.size() > 256) {
        return fail("input script has more than 256 events");
    }

    for (const nlohmann::json& entry : parsed) {
        if (!entry.is_object()) {
            return fail("input script entry is not an object");
        }
        if (!entry.contains("type") || !entry["type"].is_string()) {
            return fail("input script entry has no type");
        }
        ScriptedInput scripted;
        const std::string type = entry["type"].get<std::string>();
        double t = 0.0;
        if (entry.contains("t") && !json_number(entry["t"], &t)) {
            return fail("input script t is not a number");
        }
        scripted.t = t;

        if (type == "gamepad" || type == "gamepad_axis") {
            const bool has_axis = entry.contains("axis");
            const bool has_button = entry.contains("button");
            if (has_axis && has_button) {
                return fail("gamepad entry has both button and axis");
            }
            if (has_axis) {
                if (!entry["axis"].is_string()) {
                    return fail("gamepad axis is not a string");
                }
                const std::string axis = entry["axis"].get<std::string>();
                if (!parse_gamepad_axis(axis, &scripted.event.axis)) {
                    return fail("unknown gamepad axis: " + axis);
                }
                if (!entry.contains("value")) {
                    return fail("gamepad axis has no value");
                }
                double value = 0.0;
                if (!json_number(entry["value"], &value)) {
                    return fail("gamepad axis value is not a number");
                }
                if (value < -1.0 || value > 1.0) {
                    return fail("gamepad axis value is outside -1..1");
                }
                scripted.event.type = InputEvent::Type::GamepadAxis;
                scripted.event.value = static_cast<float>(value);
            } else {
                if (!has_button) {
                    return fail("gamepad entry has no button or axis");
                }
                if (!entry["button"].is_string()) {
                    return fail("gamepad button is not a string");
                }
                const std::string button = entry["button"].get<std::string>();
                if (!parse_gamepad_button(button, &scripted.event.button)) {
                    return fail("unknown gamepad button: " + button);
                }
                if (!entry.contains("state") || !entry["state"].is_string()) {
                    return fail("gamepad button has no state");
                }
                bool down = false;
                if (!parse_gamepad_state(entry["state"].get<std::string>(), &down)) {
                    return fail("gamepad button state must be down or up");
                }
                scripted.event.type = down ? InputEvent::Type::GamepadButtonDown
                                           : InputEvent::Type::GamepadButtonUp;
            }
            events->push_back(scripted);
            continue;
        }

        if (type == "wait") {
            if (!entry.contains("t")) {
                return fail("wait entry has no t");
            }
            scripted.event.type = InputEvent::Type::None;
            events->push_back(scripted);
            continue;
        }

        if (!parse_event_type(type, &scripted.event.type)) {
            return fail("unknown input script type: " + type);
        }
        if (entry.contains("x")) {
            double x = 0.0;
            if (!json_number(entry["x"], &x)) {
                return fail("input script x is not a number");
            }
            scripted.event.x = static_cast<float>(x);
        }
        if (entry.contains("y")) {
            double y = 0.0;
            if (!json_number(entry["y"], &y)) {
                return fail("input script y is not a number");
            }
            scripted.event.y = static_cast<float>(y);
        }
        if (entry.contains("key")) {
            if (entry["key"].is_number_integer()) {
                scripted.event.key = entry["key"].get<int>();
            } else if (entry["key"].is_string()) {
                const std::string key = entry["key"].get<std::string>();
                if (key == "Escape" || key == "escape" || key == "esc") {
                    scripted.event.key = 27;
                } else {
                    return fail("unknown input script key: " + key);
                }
            } else {
                return fail("input script key is not a number or string");
            }
        }
        events->push_back(scripted);
    }

    std::stable_sort(events->begin(), events->end(),
                     [](const ScriptedInput& a, const ScriptedInput& b) { return a.t < b.t; });
    return true;
}

bool dispatch_ui_action(UiAction action, int index, const UiActionHandlers& handlers) {
    switch (action) {
        case UiAction::None:
        case UiAction::Escape:
            return false;
        case UiAction::Call:
            if (handlers.call) {
                handlers.call();
                return true;
            }
            return false;
        case UiAction::Mute:
            if (handlers.mute) {
                handlers.mute();
                return true;
            }
            return false;
        case UiAction::End:
            if (handlers.end) {
                handlers.end();
                return true;
            }
            return false;
        case UiAction::MediaClose:
            if (handlers.media_close) {
                handlers.media_close();
                return true;
            }
            return false;
        case UiAction::MediaPrevious:
            if (handlers.media_previous) {
                handlers.media_previous();
                return true;
            }
            return false;
        case UiAction::MediaNext:
            if (handlers.media_next) {
                handlers.media_next();
                return true;
            }
            return false;
        case UiAction::SheetOption:
            if (handlers.sheet_option) {
                handlers.sheet_option(index);
                return true;
            }
            return false;
        case UiAction::SheetDismiss:
            if (handlers.sheet_dismiss) {
                handlers.sheet_dismiss();
                return true;
            }
            return false;
        case UiAction::SheetOk:
            if (handlers.sheet_ok) {
                handlers.sheet_ok();
                return true;
            }
            return false;
        case UiAction::PasskeyConfirm:
            if (handlers.passkey_confirm) {
                handlers.passkey_confirm();
                return true;
            }
            return false;
        case UiAction::DebugOpen:
            if (handlers.debug_open) {
                handlers.debug_open();
                return true;
            }
            return false;
        case UiAction::DebugClose:
            if (handlers.debug_close) {
                handlers.debug_close();
                return true;
            }
            return false;
        case UiAction::EchoMode:
            if (handlers.echo_mode) {
                handlers.echo_mode(index);
                return true;
            }
            return false;
        case UiAction::MicTest:
            if (handlers.mic_test) {
                handlers.mic_test();
                return true;
            }
            return false;
        case UiAction::SendDiagnostics:
            if (handlers.send_diagnostics) {
                handlers.send_diagnostics();
                return true;
            }
            return false;
    }
    return false;
}

}  // namespace pk
