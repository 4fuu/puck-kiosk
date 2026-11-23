#pragma once

#include "puck/puck_rig.h"
#include "puck/puck_v2.h"

#include <string>
#include <vector>

namespace pk {

// The base PUCK v2 mesh stores body, mouth, nose, and leg parts in a normalized
// rest space. The rig places them in the image-sized scene. Eye parts and arms
// are already authored in scene space.
bool is_posed_part(const std::string& name);

// Re-poses one base part into rig space. Normals and tangents are rebuilt from
// the pose Jacobian, and the per-vertex rig weights, UVs, and tangent
// handedness are preserved. Returns false for an unsupported part or an
// incomplete rig.
bool pose_puck_part(const PuckPart& part, const PuckRig& rig, std::vector<PuckVertex>* out,
                    std::string* error);

// The rig-space point for a normalized base-space position on a part.
PuckVec3 puck_part_point(const std::string& part, const PuckVec3& position, const PuckRig& rig);

}  // namespace pk
