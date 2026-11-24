#pragma once

#include "core/types.h"

namespace pk {

// The Puck's body in units of the image height, with the origin at the ground
// point and +y up. Classic's rig has a 411 px body on a 602 px image, a ground
// at 577 px, and posed body bounds of about 480 x 411 px.
struct PuckBodyGeometry {
    float center_y = 0.5075f;
    float radius_x = 0.399f;
    float radius_y = 0.341f;
};

enum class PuckGrab {
    None,
    Body,
    Turn,
};

// Internal 2D vector for the interaction; +x right, +y up.
struct PuckVec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// The web client's pointer interaction, ported from its 3D Puck physics. The
// state machine is idle/gaze-follow, pressed, grabbed (body drag with roll, or
// turn drag with yaw/tip), and falling/rising after a throw. Distances are
// fractions of the Puck's image height and +y points up.
//
// A press only arms a grab when it lands on the Puck; the drag starts after
// 10 px of movement so a tap does not move him. The app must not forward a
// press that hits a UI target.
class PuckInteraction {
public:
    void set_bounds(Rect puck, Rect stage);
    void set_geometry(const PuckBodyGeometry& geometry) { geometry_ = geometry; }
    // Clears the press, drag, throw, turn, and pointer state. The app calls
    // this when the avatar or its renderer changes; the renderers release
    // their own textures and buffers.
    void reset();

    // Feed every pointer event so the gaze follows even when the Puck is not
    // grabbed. `time` is the app's clock in seconds.
    bool pointer_down(float x, float y, float time);
    void pointer_move(float x, float y, float time);
    void pointer_up(float x, float y, float time);
    void update(float time);

    const PuckPose& pose() const { return pose_; }
    PuckGrab grab() const { return grab_; }
    bool grabbed() const { return phase_ == Phase::Grabbed; }
    bool pressed() const { return phase_ == Phase::Pressed; }
    // True while a drag or the spring-back still needs frames.
    bool active() const;

    // Test hooks.
    float linear_speed() const;
    float angular_speed() const;
    float body_roll() const { return roll_; }
    float shift_x() const { return shift_.x; }
    float shift_y() const { return shift_.y; }
    float squash() const { return squash_; }
    float turn_yaw() const { return turn_yaw_; }
    float turn_tip() const { return turn_tip_; }

private:
    enum class Phase {
        Idle,
        Pressed,
        Grabbed,
        Falling,
        Rising,
    };

    struct Sample {
        float time = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
    };

    static constexpr int kMaxSamples = 16;

    PuckVec2 to_local(float x, float y) const;
    bool inside_image(float x, float y) const;
    PuckGrab hit_test(const PuckVec2& local) const;
    void record_pointer(float x, float y, float time, bool moved);
    void add_sample(float time, float x, float y);
    PuckVec2 release_velocity(float time) const;

    void begin_grab();
    void step(float dt);
    void step_body_held(float dt);
    void step_falling(float dt);
    void step_rising(float dt);
    void step_turn(float dt);
    void step_squash(float dt);
    void write_pose(float time);
    float floor_penetration(const PuckVec2& shift, float roll) const;

    PuckBodyGeometry geometry_;

    Phase phase_ = Phase::Idle;
    PuckGrab grab_ = PuckGrab::None;

    float image_height_ = 0.0f;
    float image_width_ = 0.0f;
    float image_x_ = 0.0f;
    float image_y_ = 0.0f;
    float ground_x_ = 0.0f;
    float ground_y_ = 0.0f;
    float room_left_ = 0.0f;
    float room_right_ = 0.0f;
    float room_up_ = 0.0f;

    float press_screen_x_ = 0.0f;
    float press_screen_y_ = 0.0f;
    PuckVec2 press_local_;
    PuckVec2 finger_;
    PuckVec2 last_finger_;
    PuckVec2 velocity_;
    PuckVec2 grip_;
    PuckVec2 middle_;
    PuckVec2 shift_;
    float roll_ = 0.0f;
    float spin_ = 0.0f;
    float body_accel_x_ = 0.0f;
    float slide_ = 0.0f;
    bool landed_ = false;
    bool landing_bounce_ = false;
    int bounces_ = 0;
    float squash_ = 0.0f;
    float squash_velocity_ = 0.0f;

    float turn_yaw_ = 0.0f;
    float turn_tip_ = 0.0f;
    float turn_yaw_velocity_ = 0.0f;
    float turn_tip_velocity_ = 0.0f;
    float turn_yaw_at_grab_ = 0.0f;
    float turn_tip_at_grab_ = 0.0f;
    bool turn_home_set_ = false;
    float turn_home_ = 0.0f;

    Sample samples_[kMaxSamples];
    int sample_count_ = 0;

    float last_time_ = -1.0f;
    float last_pointer_time_ = -1.0f;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;
    PuckPose pose_;
};

}  // namespace pk
