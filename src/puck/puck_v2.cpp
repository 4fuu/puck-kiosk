#include "puck/puck_v2.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace pk {
namespace {

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPartHeaderBytes = 20;
constexpr std::size_t kVertexBytes = 64;
constexpr std::size_t kFloatsPerVertex = 16;
constexpr std::uint32_t kMaxParts = 4096;

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

float read_f32(const std::uint8_t* bytes) {
    const std::uint32_t bits = read_u32(bytes);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

bool parse_mesh(const std::uint8_t* data, std::size_t size, PuckMesh* mesh, std::string* error) {
    if (size < kHeaderBytes) {
        return fail(error, "truncated PUCK header: 12 bytes required, got " + std::to_string(size));
    }
    if (std::memcmp(data, "PUCK", 4) != 0) {
        return fail(error, "bad PUCK magic: expected \"PUCK\"");
    }

    const std::uint32_t version = read_u32(data + 4);
    if (version != 2) {
        return fail(error, "unsupported PUCK version " + std::to_string(version) + ": expected 2");
    }

    const std::uint32_t part_count = read_u32(data + 8);
    if (part_count > kMaxParts) {
        return fail(error, "implausible PUCK part count " + std::to_string(part_count));
    }
    const std::uint64_t part_table_bytes =
        static_cast<std::uint64_t>(part_count) * static_cast<std::uint64_t>(kPartHeaderBytes);
    if (part_table_bytes > static_cast<std::uint64_t>(size - kHeaderBytes)) {
        return fail(error, "truncated PUCK part table: " + std::to_string(part_count) +
                               " parts need " + std::to_string(part_table_bytes) + " bytes");
    }

    PuckMesh parsed;
    parsed.parts.reserve(part_count);

    std::size_t offset = kHeaderBytes;
    for (std::uint32_t part_index = 0; part_index < part_count; ++part_index) {
        if (size - offset < kPartHeaderBytes) {
            return fail(error, "truncated PUCK part header at byte " + std::to_string(offset));
        }

        PuckPart part;
        const std::uint8_t* name_bytes = data + offset;
        std::size_t name_length = 0;
        while (name_length < 12 && name_bytes[name_length] != 0) {
            ++name_length;
        }
        part.name.assign(reinterpret_cast<const char*>(name_bytes), name_length);

        const std::uint32_t vertex_count = read_u32(data + offset + 12);
        const std::uint32_t index_count = read_u32(data + offset + 16);
        offset += kPartHeaderBytes;

        const std::uint64_t vertex_bytes =
            static_cast<std::uint64_t>(vertex_count) * static_cast<std::uint64_t>(kVertexBytes);
        if (vertex_bytes > static_cast<std::uint64_t>(size - offset)) {
            return fail(error, "part \"" + part.name + "\" vertex count " +
                                   std::to_string(vertex_count) + " exceeds the remaining " +
                                   std::to_string(size - offset) + " bytes");
        }

        part.vertices.resize(vertex_count);
        for (std::uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
            const std::uint8_t* vertex_bytes_ptr = data + offset + static_cast<std::size_t>(vertex_index) * kVertexBytes;
            float values[kFloatsPerVertex];
            for (std::size_t i = 0; i < kFloatsPerVertex; ++i) {
                values[i] = read_f32(vertex_bytes_ptr + i * sizeof(float));
            }
            for (std::size_t i = 0; i < kFloatsPerVertex; ++i) {
                if (!std::isfinite(values[i])) {
                    return fail(error, "part \"" + part.name + "\" vertex " +
                                           std::to_string(vertex_index) + " has a non-finite value");
                }
            }

            PuckVertex& vertex = part.vertices[vertex_index];
            vertex.position = PuckVec3{values[0], values[1], values[2]};
            vertex.normal = PuckVec3{values[3], values[4], values[5]};
            vertex.tangent = PuckVec4{values[6], values[7], values[8], values[9]};
            vertex.uv = PuckVec2{values[10], values[11]};
            for (std::size_t i = 0; i < 4; ++i) {
                vertex.extra[i] = values[12 + i];
            }
        }
        offset += static_cast<std::size_t>(vertex_bytes);

        const std::uint64_t index_bytes = static_cast<std::uint64_t>(index_count) * 2u;
        if (index_bytes > static_cast<std::uint64_t>(size - offset)) {
            return fail(error, "part \"" + part.name + "\" index count " +
                                   std::to_string(index_count) + " exceeds the remaining " +
                                   std::to_string(size - offset) + " bytes");
        }

        part.indices.resize(index_count);
        for (std::uint32_t index = 0; index < index_count; ++index) {
            const std::uint16_t value = read_u16(data + offset + static_cast<std::size_t>(index) * 2u);
            if (value >= vertex_count) {
                return fail(error, "part \"" + part.name + "\" index " + std::to_string(index) +
                                       " references vertex " + std::to_string(value) +
                                       " but the part has " + std::to_string(vertex_count) + " vertices");
            }
            part.indices[index] = value;
        }
        offset += static_cast<std::size_t>(index_bytes);

        if ((index_count & 1u) != 0u) {
            if (size - offset < 2) {
                return fail(error, "truncated PUCK index padding for part \"" + part.name + "\"");
            }
            offset += 2;
        }

        if (!part.vertices.empty()) {
            part.bounds_min = part.vertices.front().position;
            part.bounds_max = part.vertices.front().position;
            for (const PuckVertex& vertex : part.vertices) {
                part.bounds_min.x = std::fmin(part.bounds_min.x, vertex.position.x);
                part.bounds_min.y = std::fmin(part.bounds_min.y, vertex.position.y);
                part.bounds_min.z = std::fmin(part.bounds_min.z, vertex.position.z);
                part.bounds_max.x = std::fmax(part.bounds_max.x, vertex.position.x);
                part.bounds_max.y = std::fmax(part.bounds_max.y, vertex.position.y);
                part.bounds_max.z = std::fmax(part.bounds_max.z, vertex.position.z);
            }
        }

        parsed.parts.push_back(std::move(part));
    }

    if (offset != size) {
        return fail(error, "trailing data after the last PUCK part: " + std::to_string(size - offset) + " bytes");
    }

    parsed.byte_size = size;
    *mesh = std::move(parsed);
    return true;
}

}  // namespace

std::size_t PuckMesh::vertex_count() const {
    std::size_t total = 0;
    for (const PuckPart& part : parts) {
        total += part.vertices.size();
    }
    return total;
}

std::size_t PuckMesh::index_count() const {
    std::size_t total = 0;
    for (const PuckPart& part : parts) {
        total += part.indices.size();
    }
    return total;
}

bool parse_puck_v2(const std::uint8_t* data, std::size_t size, PuckMesh* mesh, std::string* error) {
    if (mesh == nullptr) {
        return fail(error, "PUCK output mesh is null");
    }
    *mesh = PuckMesh{};
    if (data == nullptr && size != 0) {
        return fail(error, "PUCK input data is null");
    }

    try {
        return parse_mesh(data, size, mesh, error);
    } catch (const std::bad_alloc&) {
        *mesh = PuckMesh{};
        return fail(error, "out of memory while parsing PUCK mesh");
    } catch (const std::length_error&) {
        *mesh = PuckMesh{};
        return fail(error, "PUCK mesh is too large to load");
    }
}

bool load_puck_v2(const std::string& path, PuckMesh* mesh, std::string* error) {
    if (mesh == nullptr) {
        return fail(error, "PUCK output mesh is null");
    }
    *mesh = PuckMesh{};

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return fail(error, "cannot open PUCK file \"" + path + "\"");
    }
    const std::streamoff end = file.tellg();
    if (end < 0) {
        return fail(error, "cannot determine the size of PUCK file \"" + path + "\"");
    }
    if (static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::uint64_t>(end) > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return fail(error, "PUCK file \"" + path + "\" is too large");
    }

    const std::size_t size = static_cast<std::size_t>(end);
    try {
        std::vector<std::uint8_t> bytes(size);
        if (size != 0) {
            file.seekg(0, std::ios::beg);
            if (!file) {
                return fail(error, "cannot seek PUCK file \"" + path + "\"");
            }
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
            if (file.gcount() != static_cast<std::streamsize>(size)) {
                return fail(error, "failed to read PUCK file \"" + path + "\"");
            }
        }
        return parse_puck_v2(bytes.data(), bytes.size(), mesh, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "out of memory while reading PUCK file \"" + path + "\"");
    }
}

}  // namespace pk
