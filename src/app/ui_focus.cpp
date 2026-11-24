#include "app/ui_focus.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pk {
namespace {

float squared_distance(const Rect& from, const Rect& to) {
    const float dx = to.center_x() - from.center_x();
    const float dy = to.center_y() - from.center_y();
    return dx * dx + dy * dy;
}

}  // namespace

void UiFocus::sync() {
    refresh_targets();
}

void UiFocus::reset() {
    targets_.clear();
    focused_ = -1;
    focus_action_ = UiAction::None;
    focus_index_ = -1;
    cancel_hold();
    axis_active_[0] = false;
    axis_active_[1] = false;
}

void UiFocus::refresh_targets() {
    targets_.clear();
    if (map_) {
        for (const UiTarget& target : map_->targets()) {
            if (target.action == UiAction::None) {
                continue;
            }
            // A later blocker or overlapping target wins the point at this
            // target's center, which means the target is not visible or not
            // reachable by a click either.
            const UiTarget* top =
                map_->hit(target.rect.center_x(), target.rect.center_y());
            if (!top || top->action == UiAction::None || top->action != target.action ||
                top->index != target.index) {
                continue;
            }
            targets_.push_back(target);
        }
    }
    if (focus_action_ == UiAction::None) {
        focused_ = -1;
        return;
    }
    const int found = find_target(focus_action_, focus_index_);
    if (found >= 0) {
        focused_ = found;
        return;
    }
    if (!targets_.empty()) {
        focused_ = 0;
        focus_action_ = targets_[0].action;
        focus_index_ = targets_[0].index;
        return;
    }
    focused_ = -1;
    focus_action_ = UiAction::None;
    focus_index_ = -1;
}

void UiFocus::ensure_focus() {
    if (focused_ >= 0 && focused_ < static_cast<int>(targets_.size())) {
        return;
    }
    if (targets_.empty()) {
        focused_ = -1;
        return;
    }
    focused_ = 0;
    focus_action_ = targets_[0].action;
    focus_index_ = targets_[0].index;
}

void UiFocus::focus_target(UiAction action, int index) {
    focus_action_ = action;
    focus_index_ = index;
    focused_ = find_target(action, index);
}

int UiFocus::find_target(UiAction action, int index) const {
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        if (targets_[i].action == action && targets_[i].index == index) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const UiTarget* UiFocus::target_at(int index) const {
    if (index < 0 || index >= static_cast<int>(targets_.size())) {
        return nullptr;
    }
    return &targets_[static_cast<std::size_t>(index)];
}

void UiFocus::move(FocusDirection direction) {
    cancel_hold();
    if (targets_.empty()) {
        return;
    }
    if (focused_ < 0) {
        ensure_focus();
        return;
    }

    const Rect current = targets_[static_cast<std::size_t>(focused_)].rect;
    int best = -1;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        if (static_cast<int>(i) == focused_) {
            continue;
        }
        const Rect& candidate = targets_[i].rect;
        const float dx = candidate.center_x() - current.center_x();
        const float dy = candidate.center_y() - current.center_y();
        bool ahead = false;
        switch (direction) {
            case FocusDirection::Up: ahead = dy < -1.0f; break;
            case FocusDirection::Down: ahead = dy > 1.0f; break;
            case FocusDirection::Left: ahead = dx < -1.0f; break;
            case FocusDirection::Right: ahead = dx > 1.0f; break;
        }
        if (!ahead) {
            continue;
        }
        const float distance = squared_distance(current, candidate);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(i);
        }
    }

    if (best < 0) {
        const int count = static_cast<int>(targets_.size());
        const int step =
            (direction == FocusDirection::Right || direction == FocusDirection::Down) ? 1 : -1;
        best = (focused_ + step + count) % count;
    }

    focused_ = best;
    focus_action_ = targets_[static_cast<std::size_t>(best)].action;
    focus_index_ = targets_[static_cast<std::size_t>(best)].index;
}

void UiFocus::begin_hold(UiAction action, int index, float now, bool pointer) {
    hold_active_ = true;
    hold_pointer_ = pointer;
    hold_action_ = action;
    hold_index_ = index;
    hold_start_ = now;
    focus_target(action, index);
}

void UiFocus::cancel_hold() {
    hold_active_ = false;
    hold_pointer_ = false;
    hold_action_ = UiAction::None;
    hold_index_ = -1;
}

float UiFocus::hold_progress(float now) const {
    if (!hold_active_) {
        return 0.0f;
    }
    return std::clamp((now - hold_start_) / kHoldSeconds, 0.0f, 1.0f);
}

UiFocusResult UiFocus::update(float now) {
    UiFocusResult result;
    if (combo_start_ >= 0.0f && now - combo_start_ >= 1.5f) {
        combo_start_ = -1.0f;
        result.toggle_debug = true;
    }
    if (!hold_active_) {
        return result;
    }
    refresh_targets();
    if (find_target(hold_action_, hold_index_) < 0) {
        cancel_hold();
        return result;
    }
    if (hold_progress(now) >= 1.0f) {
        result.action = hold_action_;
        result.index = hold_index_;
        hold_active_ = false;
        hold_pointer_ = false;
        hold_action_ = UiAction::None;
        hold_index_ = -1;
    }
    return result;
}

UiFocusResult UiFocus::handle(const InputEvent& event, float now) {
    UiFocusResult result;
    refresh_targets();

    switch (event.type) {
        case InputEvent::Type::GamepadButtonDown:
            result.consumed = true;
            ensure_focus();
            switch (event.button) {
                case InputEvent::GamepadButton::A: {
                    const UiTarget* target = target_at(focused_);
                    if (!target) {
                        break;
                    }
                    if (target->action == UiAction::End) {
                        begin_hold(target->action, target->index, now, false);
                    } else {
                        cancel_hold();
                        result.action = target->action;
                        result.index = target->index;
                    }
                    break;
                }
                case InputEvent::GamepadButton::B:
                case InputEvent::GamepadButton::Back:
                    cancel_hold();
                    if (event.type == InputEvent::Type::GamepadButtonDown) {
                        back_held_ = true;
                        if (start_held_) {
                            combo_start_ = now;
                        }
                    }
                    result.dismiss = true;
                    break;
                case InputEvent::GamepadButton::Start:
                    cancel_hold();
                    if (event.type == InputEvent::Type::GamepadButtonDown) {
                        start_held_ = true;
                        if (back_held_) {
                            combo_start_ = now;
                        }
                    }
                    break;
                case InputEvent::GamepadButton::DPadUp:
                    move(FocusDirection::Up);
                    break;
                case InputEvent::GamepadButton::DPadDown:
                    move(FocusDirection::Down);
                    break;
                case InputEvent::GamepadButton::DPadLeft:
                    move(FocusDirection::Left);
                    break;
                case InputEvent::GamepadButton::DPadRight:
                    move(FocusDirection::Right);
                    break;
                default:
                    break;
            }
            break;

        case InputEvent::Type::GamepadButtonUp:
            result.consumed = true;
            if (event.button == InputEvent::GamepadButton::A && hold_active_ && !hold_pointer_) {
                cancel_hold();
            }
            if (event.button == InputEvent::GamepadButton::Back) {
                back_held_ = false;
                combo_start_ = -1.0f;
            }
            if (event.button == InputEvent::GamepadButton::Start) {
                start_held_ = false;
                combo_start_ = -1.0f;
            }
            break;

        case InputEvent::Type::GamepadAxis: {
            result.consumed = true;
            const int axis = event.axis == InputEvent::GamepadAxis::LeftX ? 0 : 1;
            const bool active = std::fabs(event.value) >= kAxisThreshold;
            if (active && !axis_active_[axis]) {
                ensure_focus();
                if (axis == 0) {
                    move(event.value > 0.0f ? FocusDirection::Right : FocusDirection::Left);
                } else {
                    // evdev reports up as a negative Y value.
                    move(event.value < 0.0f ? FocusDirection::Up : FocusDirection::Down);
                }
            }
            axis_active_[axis] = active;
            break;
        }

        case InputEvent::Type::PointerDown: {
            const UiTarget* target = map_ ? map_->hit(event.x, event.y) : nullptr;
            if (target && target->action == UiAction::End) {
                begin_hold(target->action, target->index, now, true);
                result.consumed = true;
            }
            break;
        }

        case InputEvent::Type::PointerUp:
            if (hold_active_ && hold_pointer_) {
                cancel_hold();
                result.consumed = true;
            }
            break;

        case InputEvent::Type::None:
        case InputEvent::Type::PointerMove:
        case InputEvent::Type::KeyDown:
        case InputEvent::Type::KeyUp:
        case InputEvent::Type::Quit:
            break;
    }
    return result;
}

UiFocusView UiFocus::view(float now, bool visible) const {
    UiFocusView view;
    view.visible = visible;
    const UiTarget* target = target_at(focused_);
    if (!target) {
        return view;
    }
    view.focused = true;
    view.action = target->action;
    view.index = target->index;
    view.holding = hold_active_;
    view.hold_progress = hold_progress(now);
    return view;
}

UiAction UiFocus::focused_action() const {
    const UiTarget* target = target_at(focused_);
    return target ? target->action : UiAction::None;
}

int UiFocus::focused_index() const {
    const UiTarget* target = target_at(focused_);
    return target ? target->index : -1;
}

const Rect* UiFocus::focused_rect() const {
    const UiTarget* target = target_at(focused_);
    return target ? &target->rect : nullptr;
}

}  // namespace pk
