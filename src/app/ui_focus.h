#pragma once

#include "app/ui_input.h"
#include "platform/display.h"

#include <vector>

namespace pk {

enum class FocusDirection {
    Up,
    Down,
    Left,
    Right,
};

// What the app should do with one event the focus layer handled. `consumed`
// means the event belongs to the focus UI and must not reach UiInputRouter.
struct UiFocusResult {
    UiAction action = UiAction::None;
    int index = -1;
    bool dismiss = false;
    bool toggle_debug = false;
    bool consumed = false;
};

// What CarUi needs to draw the focus ring, the hold ring, and the hint bar.
struct UiFocusView {
    bool focused = false;
    bool visible = false;
    UiAction action = UiAction::None;
    int index = -1;
    bool holding = false;
    float hold_progress = 0.0f;
};

// Gamepad focus over the actionable rects of the last UiHitMap frame. DPad and
// the left stick move to the nearest rect in that direction, with a draw-order
// cycle when nothing lies that way. A activates, B asks the app to dismiss the
// topmost sheet/overlay, and Start toggles Debug. End is hold-to-confirm on
// every input: A, pointer, and touch all complete after kHoldSeconds.
class UiFocus {
public:
    static constexpr float kHoldSeconds = 0.9f;
    static constexpr float kAxisThreshold = 0.5f;

    void set_hit_map(const UiHitMap* map) { map_ = map; }

    // Rebuilds the focusable list from the current hit map and keeps the
    // current target when it still exists. Call after each draw.
    void sync();

    // Clears the current target and any hold. Used when the screen mode
    // changes so the first gamepad event starts from the new screen.
    void reset();

    // Handles one input event at `now` seconds.
    UiFocusResult handle(const InputEvent& event, float now);

    // Advances the hold timer. Returns End once when a hold completes.
    UiFocusResult update(float now);

    UiFocusView view(float now, bool visible) const;

    bool focused() const { return focused_ >= 0; }
    UiAction focused_action() const;
    int focused_index() const;
    const Rect* focused_rect() const;

    bool hold_active() const { return hold_active_; }
    float hold_progress(float now) const;

    void cancel_hold();

private:
    void refresh_targets();
    void ensure_focus();
    void move(FocusDirection direction);
    void begin_hold(UiAction action, int index, float now, bool pointer);
    void focus_target(UiAction action, int index);
    int find_target(UiAction action, int index) const;
    const UiTarget* target_at(int index) const;

    const UiHitMap* map_ = nullptr;
    std::vector<UiTarget> targets_;
    int focused_ = -1;
    UiAction focus_action_ = UiAction::None;
    int focus_index_ = -1;

    bool hold_active_ = false;
    bool hold_pointer_ = false;
    UiAction hold_action_ = UiAction::None;
    int hold_index_ = -1;
    float hold_start_ = 0.0f;

    // Back+Start held together for 1.5s reveals the hidden Debug entry.
    bool back_held_ = false;
    bool start_held_ = false;
    float combo_start_ = -1.0f;

    bool axis_active_[2] = {false, false};
};

}  // namespace pk
