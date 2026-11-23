#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pk {

struct PuckVec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct PuckVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct PuckVec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// A PUCK v2 vertex is 16 floats: position, normal, tangent (w is the
// handedness), UV, and four trailing floats whose meaning is not needed for
// mesh bounds. The trailing values are kept verbatim for the renderer.
struct PuckVertex {
    PuckVec3 position;
    PuckVec3 normal;
    PuckVec4 tangent;
    PuckVec2 uv;
    float extra[4] = {};
};

static_assert(sizeof(float) == 4, "PUCK v2 vertices store 32-bit floats");
static_assert(sizeof(PuckVertex) == 64, "PUCK v2 vertices are 16 floats");

struct PuckPart {
    std::string name;
    std::vector<PuckVertex> vertices;
    std::vector<std::uint16_t> indices;
    PuckVec3 bounds_min;
    PuckVec3 bounds_max;
};

struct PuckMesh {
    std::vector<PuckPart> parts;
    std::size_t byte_size = 0;

    std::size_t vertex_count() const;
    std::size_t index_count() const;
};

// Parses a PUCK v2 container. On failure returns false and leaves `mesh`
// empty. `error`, when non-null, receives a human-readable reason.
bool parse_puck_v2(const std::uint8_t* data, std::size_t size, PuckMesh* mesh, std::string* error);
bool load_puck_v2(const std::string& path, PuckMesh* mesh, std::string* error);

}  // namespace pk
