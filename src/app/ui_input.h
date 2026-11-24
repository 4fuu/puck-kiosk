#pragma once

#include "core/types.h"
#include "platform/display.h"

#include <functional>
#include <string>
#include <vector>

namespace pk {

// Actions the car UI can emit. A screen adds targets with these values while
// it draws; the app maps each action to the controller that owns it.
enum class UiAction {
    None,
    Call,
    Mute,
    End,
    MediaClose,
    MediaPrevious,
    MediaNext,
    SheetOption,
    SheetDismiss,
    SheetOk,
    PasskeyConfirm,
    DebugOpen,
    DebugClose,
    EchoMode,
    MicTest,
    SendDiagnostics,
    Escape,
};

const char* ui_action_name(UiAction action);

struct UiTarget {
    Rect rect;
    UiAction action = UiAction::None;
    // Option index for SheetOption, echo mode index for EchoMode.
    int index = -1;
};

// The rects of the buttons that were drawn this frame. Targets are added in
// draw order, so the last one under a point is the topmost and wins.
class UiHitMap {
public:
    void clear() { targets_.clear(); }
    void add(Rect rect, UiAction action, int index = -1);

    // Returns the topmost target containing the point, or nullptr. A target
    // with UiAction::None is a blocker: it consumes the click.
    const UiTarget* hit(float x, float y) const;

    const std::vector<UiTarget>& targets() const { return targets_; }
    bool empty() const { return targets_.empty(); }

private:
    std::vector<UiTarget> targets_;
};

bool rect_contains(Rect rect, float x, float y);

// Turns press/release pairs into one action. A click fires only when the
// release lands inside the rect that was pressed, so dragging off a button
// cancels it.
class UiInputRouter {
public:
    void set_hit_map(const UiHitMap* map) { map_ = map; }

    // Returns the completed action, or None. Escape is returned on key down.
    UiAction handle(const InputEvent& event, int* index = nullptr);

    bool quit_requested() const { return quit_; }
    bool pressed() const { return armed_; }
    void reset();

private:
    const UiHitMap* map_ = nullptr;
    bool armed_ = false;
    UiTarget pressed_;
    bool quit_ = false;
};

// Scripted input for headless verification. `t` is kept for readability and
// ordering; the app applies entries in time order.
struct ScriptedInput {
    double t = 0.0;
    InputEvent event;
};

// Accepts a JSON array of {t, type, x, y, key}. Types: down, up, move,
// key/keydown, keyup, quit, gamepad, gamepad_axis, wait. Keys accept integers
// or "Escape". A gamepad entry is {type:"gamepad", button:"a|b|x|y|dpad_up|...",
// state:"down|up"} or {type:"gamepad", axis:"left_x|left_y", value:-1..1}.
// "wait" advances the focus clock to t without producing an event.
bool parse_input_script(const std::string& text, std::vector<ScriptedInput>* events,
                        std::string* error);

// The callbacks a screen provides for the actions its targets emit. Missing
// callbacks are no-ops.
struct UiActionHandlers {
    std::function<void()> call;
    std::function<void()> mute;
    std::function<void()> end;
    std::function<void()> media_close;
    std::function<void()> media_previous;
    std::function<void()> media_next;
    std::function<void(int)> sheet_option;
    std::function<void()> sheet_dismiss;
    std::function<void()> sheet_ok;
    std::function<void()> passkey_confirm;
    std::function<void()> debug_open;
    std::function<void()> debug_close;
    std::function<void(int)> echo_mode;
    std::function<void()> mic_test;
    std::function<void()> send_diagnostics;
};

// Returns true when a handler ran.
bool dispatch_ui_action(UiAction action, int index, const UiActionHandlers& handlers);

}  // namespace pk
