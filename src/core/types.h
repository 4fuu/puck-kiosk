#pragma once

#include <cstdint>
#include <string>

namespace pk {

struct Size {
    int width = 0;
    int height = 0;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float center_x() const { return x + w * 0.5f; }
    float center_y() const { return y + h * 0.5f; }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

constexpr Color hex(std::uint32_t value, float alpha = 1.0f) {
    return Color{
        static_cast<float>((value >> 16) & 0xffu) / 255.0f,
        static_cast<float>((value >> 8) & 0xffu) / 255.0f,
        static_cast<float>(value & 0xffu) / 255.0f,
        alpha,
    };
}

enum class AppState {
    Pairing,
    Car,
    Calling,
    Media,
};

enum class PuckPhase {
    Idle,
    Connecting,
    Listening,
    Speaking,
    Thinking,
    Paused,
    Failed,
};

// The pointer-interaction pose, in units of the Puck's image height with +y
// up. The renderers apply it on top of their own breathing and gaze animation;
// a zero pose is the resting pose.
struct PuckPose {
    float shift_x = 0.0f;
    float lift_y = 0.0f;
    float roll = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float squash = 0.0f;
    bool grabbed = false;
    // True while the pointer has moved recently and should attract the gaze.
    bool pointer_active = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
};

struct PuckVisual {
    bool speaking = false;
    bool listening = false;
    bool thinking = false;
    bool not_listening = false;
    float mouth = 0.0f;
    PuckPose pose;
};

enum class CallPhase {
    Idle,
    Starting,
    Connecting,
    Active,
    Speaking,
    Thinking,
    Muted,
    Ended,
    Failed,
};

struct CallView {
    CallPhase phase = CallPhase::Idle;
    std::string caption = "Call Puck";
    std::string detail;
    bool muted = false;
    bool speaking = false;
    bool thinking = false;
    float input_level = 0.0f;
    float output_level = 0.0f;
    float mouth = 0.0f;
    int jitter_ms = 0;

    bool in_call() const {
        return phase == CallPhase::Starting || phase == CallPhase::Connecting ||
               phase == CallPhase::Active || phase == CallPhase::Speaking ||
               phase == CallPhase::Thinking || phase == CallPhase::Muted;
    }
};

struct Avatar {
    std::string id = "classic";
    std::string image_path;
};

}  // namespace pk
