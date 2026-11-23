#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace pk {

struct PuckRigBody {
    float bottom = 0.0f;
    float height = 0.0f;
    float depth = 0.0f;
    std::vector<float> k;
    std::vector<float> cx;
};

struct PuckRigLeg {
    float pivot[3] = {};
    float axis[3] = {};
    float radius = 0.0f;
    float stretch = 0.0f;
};

struct PuckRigArm {
    float joints[3][3] = {};
};

struct PuckRigLimbs {
    PuckRigLeg leg_l;
    PuckRigLeg leg_r;
    PuckRigArm arm_l;
    PuckRigArm arm_r;
};

struct PuckRigNose {
    float matrix[12] = {};
    float length = 0.0f;
};

struct PuckRigEye {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 0.0f;
};

struct PuckRigMouth {
    float x = 0.0f;
    float y = 0.0f;
    float half_width = 0.0f;
    float t = 0.0f;
    float angle = 0.0f;
    float center = 0.0f;
    float curve_px = 0.0f;
    float curve = 0.0f;
};

struct PuckRigGround {
    float x = 0.0f;
    float y = 0.0f;
};

struct PuckRigMaterial {
    float gloss = 0.0f;
    float height_range[2] = {};
    float rim = 0.0f;
};

struct PuckRig {
    std::string id;
    int width = 0;
    int height = 0;
    PuckRigBody body;
    PuckRigLimbs limbs;
    PuckRigNose nose;
    std::vector<PuckRigEye> eyes;
    PuckRigMouth mouth;
    PuckRigGround ground;
    Color skin_color;
    Color sclera_color;
    PuckRigMaterial material;
};

// Parses the rig JSON. On failure returns false and leaves `rig` empty.
// `error`, when non-null, receives a human-readable reason.
bool parse_puck_rig(const std::string& json, PuckRig* rig, std::string* error);
bool load_puck_rig(const std::string& path, PuckRig* rig, std::string* error);

}  // namespace pk
