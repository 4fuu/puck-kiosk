#include "puck/puck_3d_pose.h"

#include <algorithm>
#include <cmath>

namespace pk {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMouthCornerAngle = 18.0f * kPi / 180.0f;
constexpr float kPoseEpsilon = 0.001f;

float clamp01(float value) {
    return std::min(1.0f, std::max(0.0f, value));
}

float sample_spline(const std::vector<float>& samples, float t) {
    const std::size_t count = samples.size();
    if (count == 0) {
        return 0.0f;
    }
    if (count == 1) {
        return samples[0];
    }
    const float position = clamp01(t) * static_cast<float>(count - 1);
    const std::size_t index = std::min(
        count - 2, static_cast<std::size_t>(std::max(0.0f, std::floor(position))));
    const float f = position - static_cast<float>(index);
    const auto at = [&](long long i) {
        return samples[static_cast<std::size_t>(std::min<long long>(
            static_cast<long long>(count) - 1, std::max<long long>(0, i)))];
    };
    const float s = at(static_cast<long long>(index) - 1);
    const float c = at(static_cast<long long>(index));
    const float l = at(static_cast<long long>(index) + 1);
    const float u = at(static_cast<long long>(index) + 2);
    return 0.5f *
           (2.0f * c + (-s + l) * f + (2.0f * s - 5.0f * c + 4.0f * l - u) * f * f +
            (-s + 3.0f * c - 3.0f * l + u) * f * f * f);
}

// A smooth two-segment ease used by the mouth shape: from 0 to `peak` over the
// first segment, then to `end` over the second.
float ease(float value, float split, float peak, float end) {
    const float start_bias = std::min(1.0f, std::max(-1.0f, 1.0f - split / peak));
    const float end_bias = std::min(1.0f, std::max(-1.0f, (end - split) / (end - peak) - 1.0f));
    if (value <= split) {
        const float r = std::min(1.0f, std::max(0.0f, value / split));
        return peak * (r + start_bias * r * (1.0f - r));
    }
    const float r = std::min(1.0f, std::max(0.0f, (value - split) / (end - split)));
    return peak + (end - peak) * (r + end_bias * r * (1.0f - r));
}

// The body is a lathe: (x, y) in the normalized base mesh selects a point on
// the mouth/eye cross-section, and the rig's k/cx samples turn that into an
// image-sized body.
PuckVec3 mouth_point(const PuckVec3& p, const PuckRig& rig) {
    const PuckRigMouth& mouth = rig.mouth;
    const float y = p.y;
    const float angle = std::atan2(p.x, p.z);
    const float radius = std::hypot(p.x, p.z);
    float open = ease(y, 0.55f, mouth.t, 1.0f);
    const float abs_angle = std::abs(angle);
    float shape_angle = 0.0f;
    if (abs_angle <= kMouthCornerAngle) {
        shape_angle = abs_angle * mouth.angle / kMouthCornerAngle;
    } else {
        shape_angle = kPi - ease(kPi - abs_angle, kPi - kMouthCornerAngle, kPi - mouth.angle, kPi);
    }
    float theta = angle < 0.0f ? -shape_angle : shape_angle;
    theta += mouth.center * std::cos(theta * 0.5f) * std::cos(theta * 0.5f);
    const float u = (theta - mouth.center) / mouth.angle;
    const float curve_weight =
        (u * u * std::exp(-std::pow(u / 1.3f, 4.0f))) / 0.7012f;
    open += mouth.curve * curve_weight * std::exp(-std::pow((open - mouth.t) / 0.12f, 2.0f));

    const float f = y * (1.0f - y);
    const float spread = f > 1e-9f ? std::sqrt(std::max(0.0f, open * (1.0f - open)) / f) : 1.0f;
    const float profile = 2.0f;
    const float half_width =
        radius * spread *
        std::pow(std::pow(std::abs(std::sin(theta)), profile) +
                     std::pow(std::abs(std::cos(theta)), profile),
                 -1.0f / profile);
    return PuckVec3{half_width * std::sin(theta), open, half_width * std::cos(theta)};
}

PuckVec3 body_point(const PuckVec3& p, const PuckRig& rig) {
    const PuckVec3 section = mouth_point(p, rig);
    const float width = sample_spline(rig.body.k, section.y) * rig.body.height;
    return PuckVec3{
        sample_spline(rig.body.cx, section.y) + section.x * width,
        -rig.body.bottom + section.y * rig.body.height,
        section.z * width * rig.body.depth,
    };
}

// The rig's leg axis defines a frame: the normalized axis is the leg's length
// direction, and the other two axes are the cross-section.
void leg_frame(const PuckRigLeg& leg, float frame[3][3]) {
    const float length = std::sqrt(leg.axis[0] * leg.axis[0] + leg.axis[1] * leg.axis[1] +
                                   leg.axis[2] * leg.axis[2]);
    const float inverse = length > 1e-9f ? 1.0f / length : 0.0f;
    const float n[3] = {-leg.axis[0] * inverse, -leg.axis[1] * inverse, -leg.axis[2] * inverse};
    float r[3] = {n[1], -n[0], 0.0f};
    const float r_length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (r_length > 1e-9f) {
        r[0] /= r_length;
        r[1] /= r_length;
        r[2] /= r_length;
    }
    frame[0][0] = r[0];
    frame[0][1] = r[1];
    frame[0][2] = r[2];
    frame[1][0] = n[0];
    frame[1][1] = n[1];
    frame[1][2] = n[2];
    frame[2][0] = r[1] * n[2] - r[2] * n[1];
    frame[2][1] = r[2] * n[0] - r[0] * n[2];
    frame[2][2] = r[0] * n[1] - r[1] * n[0];
}

PuckVec3 leg_point(const PuckVec3& p, const PuckRigLeg& leg) {
    float stretch = p.y;
    if (p.y <= 0.0f) {
        stretch = p.y > -6.0f ? p.y * leg.stretch : -6.0f * leg.stretch + (p.y + 6.0f);
    }
    const float local[3] = {p.x * leg.radius, stretch * leg.radius, p.z * leg.radius};
    float frame[3][3];
    leg_frame(leg, frame);
    return PuckVec3{
        leg.pivot[0] + frame[0][0] * local[0] + frame[1][0] * local[1] + frame[2][0] * local[2],
        leg.pivot[1] + frame[0][1] * local[0] + frame[1][1] * local[1] + frame[2][1] * local[2],
        leg.pivot[2] + frame[0][2] * local[0] + frame[1][2] * local[1] + frame[2][2] * local[2],
    };
}

PuckVec3 nose_point(const PuckVec3& p, const PuckRig& rig) {
    const float* m = rig.nose.matrix;
    return PuckVec3{
        m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
        m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
        m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11],
    };
}

bool part_uses_body(const std::string& name) {
    return name == "body" || name == "mouth";
}

bool part_uses_leg(const std::string& name) {
    return name == "legL" || name == "legR";
}

const PuckRigLeg* leg_for(const std::string& name, const PuckRig& rig) {
    if (name == "legL") {
        return &rig.limbs.leg_l;
    }
    if (name == "legR") {
        return &rig.limbs.leg_r;
    }
    return nullptr;
}

PuckVec3 normalize(const PuckVec3& v, const PuckVec3& fallback) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-9f) {
        return fallback;
    }
    return PuckVec3{v.x / length, v.y / length, v.z / length};
}

PuckVec3 transform_normal(const float jacobian[3][3], const PuckVec3& n) {
    const float c00 = jacobian[1][1] * jacobian[2][2] - jacobian[1][2] * jacobian[2][1];
    const float c01 = -(jacobian[1][0] * jacobian[2][2] - jacobian[1][2] * jacobian[2][0]);
    const float c02 = jacobian[1][0] * jacobian[2][1] - jacobian[1][1] * jacobian[2][0];
    const float c10 = -(jacobian[0][1] * jacobian[2][2] - jacobian[0][2] * jacobian[2][1]);
    const float c11 = jacobian[0][0] * jacobian[2][2] - jacobian[0][2] * jacobian[2][0];
    const float c12 = -(jacobian[0][0] * jacobian[2][1] - jacobian[0][1] * jacobian[2][0]);
    const float c20 = jacobian[0][1] * jacobian[1][2] - jacobian[0][2] * jacobian[1][1];
    const float c21 = -(jacobian[0][0] * jacobian[1][2] - jacobian[0][2] * jacobian[1][0]);
    const float c22 = jacobian[0][0] * jacobian[1][1] - jacobian[0][1] * jacobian[1][0];
    const float determinant =
        jacobian[0][0] * c00 + jacobian[0][1] * c01 + jacobian[0][2] * c02;
    PuckVec3 result{
        c00 * n.x + c01 * n.y + c02 * n.z,
        c10 * n.x + c11 * n.y + c12 * n.z,
        c20 * n.x + c21 * n.y + c22 * n.z,
    };
    if (determinant < 0.0f) {
        result.x = -result.x;
        result.y = -result.y;
        result.z = -result.z;
    }
    return result;
}

PuckVec3 transform_tangent(const float jacobian[3][3], const PuckVec3& t) {
    return PuckVec3{
        jacobian[0][0] * t.x + jacobian[0][1] * t.y + jacobian[0][2] * t.z,
        jacobian[1][0] * t.x + jacobian[1][1] * t.y + jacobian[1][2] * t.z,
        jacobian[2][0] * t.x + jacobian[2][1] * t.y + jacobian[2][2] * t.z,
    };
}

}  // namespace

bool is_posed_part(const std::string& name) {
    return part_uses_body(name) || name == "nose" || part_uses_leg(name);
}

PuckVec3 puck_part_point(const std::string& part, const PuckVec3& position, const PuckRig& rig) {
    if (part_uses_body(part)) {
        return body_point(position, rig);
    }
    if (part == "nose") {
        return nose_point(position, rig);
    }
    if (const PuckRigLeg* leg = leg_for(part, rig)) {
        return leg_point(position, *leg);
    }
    return position;
}

bool pose_puck_part(const PuckPart& part, const PuckRig& rig, std::vector<PuckVertex>* out,
                    std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!out) {
        return fail("pose_puck_part: no output");
    }
    if (!is_posed_part(part.name)) {
        return fail("pose_puck_part: part \"" + part.name + "\" is already in scene space");
    }
    if (part_uses_body(part.name)) {
        if (rig.body.k.size() < 2 || rig.body.cx.size() < 2 || rig.body.height <= 0.0f) {
            return fail("pose_puck_part: rig has no body profile for \"" + part.name + "\"");
        }
    } else if (part.name == "nose") {
        if (rig.nose.length <= 0.0f) {
            return fail("pose_puck_part: rig has no nose matrix");
        }
    } else if (const PuckRigLeg* leg = leg_for(part.name, rig)) {
        if (leg->radius <= 0.0f) {
            return fail("pose_puck_part: rig has no leg for \"" + part.name + "\"");
        }
    }

    out->resize(part.vertices.size());
    for (std::size_t i = 0; i < part.vertices.size(); ++i) {
        const PuckVertex& source = part.vertices[i];
        const PuckVec3 p = source.position;
        float jacobian[3][3] = {};
        for (int axis = 0; axis < 3; ++axis) {
            PuckVec3 forward = p;
            PuckVec3 backward = p;
            float* f = axis == 0 ? &forward.x : (axis == 1 ? &forward.y : &forward.z);
            float* b = axis == 0 ? &backward.x : (axis == 1 ? &backward.y : &backward.z);
            *f += kPoseEpsilon;
            *b -= kPoseEpsilon;
            const PuckVec3 plus = puck_part_point(part.name, forward, rig);
            const PuckVec3 minus = puck_part_point(part.name, backward, rig);
            jacobian[0][axis] = (plus.x - minus.x) / (2.0f * kPoseEpsilon);
            jacobian[1][axis] = (plus.y - minus.y) / (2.0f * kPoseEpsilon);
            jacobian[2][axis] = (plus.z - minus.z) / (2.0f * kPoseEpsilon);
        }

        PuckVertex posed = source;
        posed.position = puck_part_point(part.name, p, rig);
        posed.normal = normalize(transform_normal(jacobian, source.normal), source.normal);
        const PuckVec3 tangent =
            normalize(transform_tangent(jacobian, PuckVec3{source.tangent.x, source.tangent.y,
                                                           source.tangent.z}),
                      PuckVec3{source.tangent.x, source.tangent.y, source.tangent.z});
        posed.tangent.x = tangent.x;
        posed.tangent.y = tangent.y;
        posed.tangent.z = tangent.z;
        (*out)[i] = posed;
    }
    return true;
}

}  // namespace pk
