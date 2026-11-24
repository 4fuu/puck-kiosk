#include "puck/puck_interaction.h"

#include <algorithm>
#include <cmath>

namespace pk {
namespace {

// Extracted from the web's 3D Puck chunk (99ba531a48b12780.js). The comment on
// each value is the minified name it had there.
constexpr float kPi = 3.14159265358979323846f;
constexpr float kImageAspect = 498.0f / 602.0f;
constexpr float kGroundX = 248.47088898442826f / 498.0f;
constexpr float kGroundY = 577.0f / 602.0f;

constexpr float kDragThresholdPx = 10.0f;  // `Math.hypot(r, i) < 10`
constexpr float kGrabSlopPx = 10.0f;       // `n.length(10)`
constexpr float kPointerWindow = 0.1f;     // `e.timeStamp - 100`
constexpr float kPointerIdle = 3.0f;       // `performance.now() - pointerSeenAt > 3e3`
constexpr float kTurnScale = 0.42f;        // `Be(e) = e.puck.height * 0.42`
constexpr float kTipLimit = 0.45f;         // `U = 0.45`
constexpr float kMaxTurnSpeed = 25.0f;     // `Se = 25`
constexpr float kYawSpring = 7.0f;         // `be = 7`
constexpr float kTipSpring = 9.0f;         // `xe = 9`
constexpr float kFreeSpinSpeed = 1.2f;     // `ye = 1.2`
constexpr float kBodySlope = 0.6f;         // `Ce = 0.6`
constexpr float kRollLimit = 1.2f;         // `K(l.roll + l.spin * t, -1.2, 1.2)`
constexpr float kVelocityFollow = 25.0f;   // `1 - Math.exp(-25 * t)`
constexpr float kSpinSpring = 36.0f;       // `-36 * l.roll`
constexpr float kSpinDamping = 3.0f;       // `-3 * l.spin`
constexpr float kSpinLever = 0.35f;        // `/(0.35 * a)`
constexpr float kThrowLimit = 6.0f;        // `let e = 6 * t.puck.height`
constexpr float kSpinLimit = 10.0f;        // `K(r.spin, -10, 10)`
constexpr float kGravity = 9.0f;           // `we = 9`
constexpr float kSpinDecay = 0.5f;         // `l.spin *= Math.exp(-0.5 * t)`
constexpr float kWallBounce = -0.4f;       // `l.velocity[0] *= -0.4`
constexpr float kBounceRestitution = 0.65f;  // `De = 0.65`
constexpr float kBounceThreshold = 1.5f;   // `Ee = 1.5`
constexpr float kBounceMinImpact = 1.0f;   // `Oe = 1`
constexpr int kBounceCount = 3;            // `l.bounces = 3`
constexpr float kToppleRoll = 0.6f;        // `Te = 0.6`
constexpr float kRiseRollOmega = 6.0f;     // `Ue(l.roll, l.spin, 6, 0.8, t)`
constexpr float kRiseRollDamping = 0.8f;
constexpr float kRiseSlideOmega = 4.0f;    // `Ue(l.slide, l.velocity[0], 4, 1, t)`
constexpr float kRiseSlideDamping = 1.0f;
constexpr float kSquashOmega = 14.0f;      // `Ue(l.squash, l.squashVelocity, 14, 0.3, t)`
constexpr float kSquashDamping = 0.3f;
constexpr float kSettleDistancePx = 0.5f;  // `Math.abs(l.shift[0]) < 0.5`
constexpr float kSettleAngle = 0.005f;     // `Math.abs(l.roll) < 0.005`
constexpr float kSettleSpin = 0.05f;       // `Math.abs(l.spin) < 0.05`

float clamp(float value, float low, float high) {
    return std::min(high, std::max(low, value));
}

PuckVec2 add(const PuckVec2& a, const PuckVec2& b) {
    return {a.x + b.x, a.y + b.y};
}

PuckVec2 sub(const PuckVec2& a, const PuckVec2& b) {
    return {a.x - b.x, a.y - b.y};
}

PuckVec2 scale(const PuckVec2& value, float factor) {
    return {value.x * factor, value.y * factor};
}

PuckVec2 rotate(const PuckVec2& value, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {value.x * c - value.y * s, value.x * s + value.y * c};
}

// `He(e, t, n, r)`: the exact critically damped spring used for the yaw and
// tip return.
void spring_exact(float& value, float& velocity, float omega, float dt) {
    const float decay = std::exp(-omega * dt);
    const float offset = velocity + omega * value;
    value = (value + offset * dt) * decay;
    velocity = (velocity - omega * offset * dt) * decay;
}

// `Ue(e, t, n, r, i)`: the damped spring used by the rising and squash phases.
void spring_damped(float& value, float& velocity, float omega, float damping, float dt) {
    const int steps = std::max(1, static_cast<int>(std::ceil(dt * omega / 0.3f)));
    const float step = dt / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        velocity += (-omega * omega * value - 2.0f * damping * omega * velocity) * step;
        value += velocity * step;
    }
}

float wrap_angle(float value) {
    return value - 2.0f * kPi * std::round(value / (2.0f * kPi));
}

}  // namespace

void PuckInteraction::set_bounds(Rect puck, Rect stage) {
    image_height_ = std::min(puck.h, puck.w / kImageAspect);
    image_width_ = image_height_ * kImageAspect;
    if (image_height_ <= 0.0f || puck.w <= 0.0f || puck.h <= 0.0f) {
        image_height_ = 0.0f;
        room_left_ = 0.0f;
        room_right_ = 0.0f;
        room_up_ = 0.0f;
        return;
    }
    image_x_ = puck.center_x() - image_width_ * 0.5f;
    image_y_ = puck.center_y() - image_height_ * 0.5f;
    ground_x_ = image_x_ + image_width_ * kGroundX;
    ground_y_ = image_y_ + image_height_ * kGroundY;
    room_left_ = std::max(0.0f, (image_x_ - stage.x) / image_height_);
    room_right_ =
        std::max(0.0f, (stage.x + stage.w - (image_x_ + image_width_)) / image_height_);
    room_up_ = clamp((image_y_ - stage.y) / image_height_, 0.4f, 0.55f);
}

void PuckInteraction::reset() {
    phase_ = Phase::Idle;
    grab_ = PuckGrab::None;
    press_screen_x_ = 0.0f;
    press_screen_y_ = 0.0f;
    press_local_ = {};
    finger_ = {};
    last_finger_ = {};
    velocity_ = {};
    grip_ = {};
    middle_ = {};
    shift_ = {};
    roll_ = 0.0f;
    spin_ = 0.0f;
    body_accel_x_ = 0.0f;
    slide_ = 0.0f;
    landed_ = false;
    landing_bounce_ = false;
    bounces_ = 0;
    squash_ = 0.0f;
    squash_velocity_ = 0.0f;
    turn_yaw_ = 0.0f;
    turn_tip_ = 0.0f;
    turn_yaw_velocity_ = 0.0f;
    turn_tip_velocity_ = 0.0f;
    turn_yaw_at_grab_ = 0.0f;
    turn_tip_at_grab_ = 0.0f;
    turn_home_set_ = false;
    turn_home_ = 0.0f;
    sample_count_ = 0;
    last_time_ = -1.0f;
    last_pointer_time_ = -1.0f;
    pointer_x_ = 0.0f;
    pointer_y_ = 0.0f;
    pose_ = PuckPose{};
}

PuckVec2 PuckInteraction::to_local(float x, float y) const {
    return {(x - ground_x_) / image_height_, (ground_y_ - y) / image_height_};
}

bool PuckInteraction::inside_image(float x, float y) const {
    return image_height_ > 0.0f && x >= image_x_ && x <= image_x_ + image_width_ &&
           y >= image_y_ && y <= image_y_ + image_height_;
}

PuckGrab PuckInteraction::hit_test(const PuckVec2& local) const {
    const float slop = kGrabSlopPx / image_height_;
    const float dx = local.x / (geometry_.radius_x + slop);
    const float dy = (local.y - geometry_.center_y) / (geometry_.radius_y + slop);
    const bool in_body = dx * dx + dy * dy <= 1.0f;
    const bool lower = local.y < geometry_.center_y - std::abs(local.x) * kBodySlope;
    return in_body && lower ? PuckGrab::Body : PuckGrab::Turn;
}

void PuckInteraction::record_pointer(float x, float y, float time, bool moved) {
    pointer_x_ = x;
    pointer_y_ = y;
    if (moved) {
        last_pointer_time_ = time;
    }
}

void PuckInteraction::add_sample(float time, float x, float y) {
    if (sample_count_ < kMaxSamples) {
        samples_[sample_count_++] = Sample{time, x, y};
    } else {
        for (int i = 1; i < kMaxSamples; ++i) {
            samples_[i - 1] = samples_[i];
        }
        samples_[kMaxSamples - 1] = Sample{time, x, y};
    }
    while (sample_count_ > 2 && samples_[0].time < time - kPointerWindow) {
        for (int i = 1; i < sample_count_; ++i) {
            samples_[i - 1] = samples_[i];
        }
        --sample_count_;
    }
}

PuckVec2 PuckInteraction::release_velocity(float time) const {
    int first = 0;
    while (first < sample_count_ && samples_[first].time < time - kPointerWindow) {
        ++first;
    }
    if (first >= sample_count_ - 1) {
        return {};
    }
    const Sample& start = samples_[first];
    const Sample& end = samples_[sample_count_ - 1];
    const float span = end.time - start.time;
    if (span <= 0.0f) {
        return {};
    }
    return {(end.x - start.x) / span / image_height_,
            -(end.y - start.y) / span / image_height_};
}

bool PuckInteraction::pointer_down(float x, float y, float time) {
    update(time);
    record_pointer(x, y, time, false);
    write_pose(time);
    if (image_height_ <= 0.0f || phase_ != Phase::Idle || !inside_image(x, y)) {
        return false;
    }
    press_screen_x_ = x;
    press_screen_y_ = y;
    press_local_ = to_local(x, y);
    grab_ = hit_test(press_local_);
    phase_ = Phase::Pressed;
    sample_count_ = 0;
    add_sample(time, x, y);
    return true;
}

void PuckInteraction::pointer_move(float x, float y, float time) {
    update(time);
    record_pointer(x, y, time, true);
    add_sample(time, x, y);
    if (phase_ == Phase::Pressed) {
        const float dx = x - press_screen_x_;
        const float dy = y - press_screen_y_;
        if (std::hypot(dx, dy) >= kDragThresholdPx) {
            finger_ = to_local(x, y);
            begin_grab();
        }
    }
    if (phase_ == Phase::Grabbed) {
        finger_ = to_local(x, y);
        // Apply the direct drag now so a frame that lands on this event sees
        // the new pose instead of the previous pointer position.
        if (grab_ == PuckGrab::Turn) {
            step_turn(0.0f);
        } else {
            step_body_held(0.0f);
        }
    }
    write_pose(time);
}

void PuckInteraction::pointer_up(float x, float y, float time) {
    update(time);
    record_pointer(x, y, time, false);
    if (phase_ == Phase::Pressed) {
        phase_ = Phase::Idle;
        grab_ = PuckGrab::None;
        write_pose(time);
        return;
    }
    if (phase_ != Phase::Grabbed) {
        write_pose(time);
        return;
    }

    const PuckVec2 release = release_velocity(time);
    if (grab_ == PuckGrab::Turn) {
        turn_yaw_velocity_ = clamp(release.x / kTurnScale, -kMaxTurnSpeed, kMaxTurnSpeed);
        turn_tip_velocity_ = clamp(-release.y / kTurnScale, -kMaxTurnSpeed, kMaxTurnSpeed);
        turn_home_set_ = false;
        phase_ = Phase::Idle;
    } else {
        phase_ = Phase::Falling;
        const float speed = std::hypot(velocity_.x, velocity_.y);
        if (speed > kThrowLimit) {
            velocity_ = scale(velocity_, kThrowLimit / speed);
        }
        spin_ = clamp(spin_, -kSpinLimit, kSpinLimit);
    }
    grab_ = PuckGrab::None;
    write_pose(time);
}

void PuckInteraction::begin_grab() {
    phase_ = Phase::Grabbed;
    last_finger_ = finger_;
    velocity_ = {};
    body_accel_x_ = 0.0f;
    spin_ = 0.0f;
    landed_ = false;
    landing_bounce_ = false;
    bounces_ = 0;

    if (grab_ == PuckGrab::Turn) {
        turn_yaw_velocity_ = 0.0f;
        turn_tip_velocity_ = 0.0f;
        turn_home_set_ = false;
        turn_yaw_at_grab_ = turn_yaw_;
        // The web recovers the unclamped tip from the tanh limit before it
        // starts the drag, so the tip does not jump when the grab begins.
        const float clamped = clamp(turn_tip_ / kTipLimit, -0.99f, 0.99f);
        turn_tip_at_grab_ = kTipLimit * std::atanh(clamped);
        return;
    }

    grip_ = rotate(sub(press_local_, shift_), -roll_);
    middle_ = add(shift_, rotate(PuckVec2{0.0f, geometry_.center_y}, roll_));
}

void PuckInteraction::update(float time) {
    if (last_time_ < 0.0f) {
        last_time_ = time;
    }
    float remaining = time - last_time_;
    last_time_ = time;
    remaining = std::max(0.0f, remaining);
    // Keep the web's 0.1 s per-step cap for frame hitches, but let a scripted
    // wait advance through several capped steps instead of losing the gap.
    while (remaining > 0.0f) {
        const float chunk = std::min(remaining, 0.1f);
        step(chunk);
        remaining -= chunk;
    }
    write_pose(time);
}

void PuckInteraction::step(float dt) {
    if (phase_ == Phase::Grabbed && grab_ == PuckGrab::Body) {
        const PuckVec2 pointer_velocity = {(finger_.x - last_finger_.x) / dt,
                                       (finger_.y - last_finger_.y) / dt};
        last_finger_ = finger_;
        const PuckVec2 previous = velocity_;
        const float follow = 1.0f - std::exp(-kVelocityFollow * dt);
        velocity_.x += (pointer_velocity.x - velocity_.x) * follow;
        velocity_.y += (pointer_velocity.y - velocity_.y) * follow;
        body_accel_x_ = (velocity_.x - previous.x) / dt;
    }

    const int count =
        std::max(1, std::min(8, static_cast<int>(std::ceil(dt / (1.0f / 120.0f)))));
    const float step_dt = dt / static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        switch (phase_) {
            case Phase::Grabbed:
                if (grab_ == PuckGrab::Body) {
                    step_body_held(step_dt);
                }
                break;
            case Phase::Falling:
                step_falling(step_dt);
                break;
            case Phase::Rising:
                step_rising(step_dt);
                break;
            case Phase::Idle:
            case Phase::Pressed:
                break;
        }
        step_turn(step_dt);
        step_squash(step_dt);
    }
}

void PuckInteraction::step_body_held(float dt) {
    spin_ += (-kSpinSpring * roll_ - kSpinDamping * spin_ - body_accel_x_ / kSpinLever) * dt;
    roll_ = clamp(roll_ + spin_ * dt, -kRollLimit, kRollLimit);

    const PuckVec2 offset = rotate(grip_, roll_);
    shift_.x = clamp(finger_.x - offset.x, -room_left_, room_right_);
    shift_.y = clamp(finger_.y - offset.y, -1.0f, room_up_);
    shift_.y += std::max(0.0f, -floor_penetration(shift_, roll_));
    middle_ = add(shift_, rotate(PuckVec2{0.0f, geometry_.center_y}, roll_));
}

void PuckInteraction::step_falling(float dt) {
    velocity_.y -= kGravity * dt;
    middle_ = add(middle_, scale(velocity_, dt));
    roll_ += spin_ * dt;
    spin_ *= std::exp(-kSpinDecay * dt);
    shift_ = sub(middle_, rotate(PuckVec2{0.0f, geometry_.center_y}, roll_));

    const float clamped_x = clamp(shift_.x, -room_left_, room_right_);
    if (clamped_x != shift_.x) {
        middle_.x -= shift_.x - clamped_x;
        shift_.x = clamped_x;
        velocity_.x *= kWallBounce;
    }

    const float penetration = floor_penetration(shift_, roll_);
    if (penetration >= 0.0f) {
        return;
    }
    shift_.y -= penetration;
    middle_.y -= penetration;
    const float impact = -velocity_.y;
    roll_ = wrap_angle(roll_);
    squash_velocity_ -= std::min(0.4f, 0.12f * impact) * 14.0f;
    slide_ = clamp(shift_.x + rotate(PuckVec2{0.0f, geometry_.center_y}, roll_).x, -room_left_,
                   room_right_);

    if (!landed_) {
        landed_ = true;
        landing_bounce_ = impact > kBounceThreshold && std::abs(roll_) <= kToppleRoll;
        bounces_ = landing_bounce_ ? kBounceCount : 0;
    }
    if (landing_bounce_ && bounces_ > 0 && impact > kBounceMinImpact) {
        --bounces_;
        velocity_.y = kBounceRestitution * impact;
        spin_ = -3.0f * roll_;
        return;
    }

    phase_ = Phase::Rising;
    velocity_ = {};
    spin_ = 0.0f;
}

void PuckInteraction::step_rising(float dt) {
    spring_damped(roll_, spin_, kRiseRollOmega, kRiseRollDamping, dt);
    spring_damped(slide_, velocity_.x, kRiseSlideOmega, kRiseSlideDamping, dt);
    shift_.x = slide_ - rotate(PuckVec2{0.0f, geometry_.center_y}, roll_).x;

    const float floor = std::max(0.0f, -floor_penetration(shift_, roll_));
    shift_.y = floor + (shift_.y - floor) * std::exp(-10.0f * dt);

    const float settle_distance = kSettleDistancePx / image_height_;
    if (std::abs(roll_) < kSettleAngle && std::abs(spin_) < kSettleSpin &&
        std::abs(shift_.x) < settle_distance && std::abs(shift_.y) < settle_distance &&
        landed_) {
        phase_ = Phase::Idle;
        roll_ = 0.0f;
        spin_ = 0.0f;
        shift_ = {};
        velocity_ = {};
        slide_ = 0.0f;
    }
}

void PuckInteraction::step_turn(float dt) {
    if (phase_ == Phase::Grabbed && grab_ == PuckGrab::Turn) {
        const PuckVec2 delta = sub(finger_, press_local_);
        turn_yaw_ = turn_yaw_at_grab_ + delta.x / kTurnScale;
        const float raw_tip = turn_tip_at_grab_ - delta.y / kTurnScale;
        turn_tip_ = kTipLimit * std::tanh(raw_tip / kTipLimit);
        return;
    }

    if (!turn_home_set_ && std::abs(turn_yaw_velocity_) >= kFreeSpinSpeed) {
        turn_yaw_ += turn_yaw_velocity_ * dt;
        turn_yaw_velocity_ *= std::exp(-2.5f * dt);
    } else {
        if (!turn_home_set_) {
            turn_home_ = std::round(turn_yaw_ / (2.0f * kPi)) * (2.0f * kPi);
            turn_home_set_ = true;
        }
        float local = turn_yaw_ - turn_home_;
        spring_exact(local, turn_yaw_velocity_, kYawSpring, dt);
        turn_yaw_ = local + turn_home_;
        if (std::abs(local) < 1e-4f && std::abs(turn_yaw_velocity_) < 0.001f) {
            turn_yaw_ = 0.0f;
            turn_yaw_velocity_ = 0.0f;
            turn_home_set_ = false;
        }
    }

    spring_exact(turn_tip_, turn_tip_velocity_, kTipSpring, dt);
    if (std::abs(turn_tip_) < 1e-4f && std::abs(turn_tip_velocity_) < 0.001f) {
        turn_tip_ = 0.0f;
        turn_tip_velocity_ = 0.0f;
    }
}

void PuckInteraction::step_squash(float dt) {
    spring_damped(squash_, squash_velocity_, kSquashOmega, kSquashDamping, dt);
    squash_ = clamp(squash_, -0.6f, 0.6f);
    if (std::abs(squash_) < 1e-3f && std::abs(squash_velocity_) < 0.01f) {
        squash_ = 0.0f;
        squash_velocity_ = 0.0f;
    }
}

float PuckInteraction::floor_penetration(const PuckVec2& shift, float roll) const {
    const float body_center_y = shift.y + geometry_.center_y * std::cos(roll);
    const float radius = std::hypot(geometry_.radius_x * std::sin(roll),
                                    geometry_.radius_y * std::cos(roll));
    return std::min(shift.y, body_center_y - radius);
}

void PuckInteraction::write_pose(float time) {
    pose_.shift_x = shift_.x;
    pose_.lift_y = shift_.y;
    pose_.roll = roll_;
    pose_.yaw = turn_yaw_;
    pose_.pitch = turn_tip_;
    pose_.squash = squash_;
    pose_.grabbed = phase_ == Phase::Grabbed;
    pose_.pointer_active =
        last_pointer_time_ >= 0.0f && time - last_pointer_time_ <= kPointerIdle;
    pose_.pointer_x = pointer_x_;
    pose_.pointer_y = pointer_y_;
}

bool PuckInteraction::active() const {
    if (phase_ == Phase::Grabbed || phase_ == Phase::Falling || phase_ == Phase::Rising) {
        return true;
    }
    return std::abs(shift_.x) > 1e-4f || std::abs(shift_.y) > 1e-4f ||
           std::abs(roll_) > 1e-4f || std::abs(spin_) > 1e-3f ||
           std::abs(turn_yaw_) > 1e-3f || std::abs(turn_tip_) > 1e-3f ||
           std::abs(turn_yaw_velocity_) > 1e-3f ||
           std::abs(turn_tip_velocity_) > 1e-3f || std::abs(squash_) > 1e-3f ||
           std::abs(squash_velocity_) > 1e-3f;
}

float PuckInteraction::linear_speed() const {
    return std::hypot(velocity_.x, velocity_.y);
}

float PuckInteraction::angular_speed() const {
    return std::max(std::abs(spin_),
                    std::max(std::abs(turn_yaw_velocity_), std::abs(turn_tip_velocity_)));
}

}  // namespace pk
