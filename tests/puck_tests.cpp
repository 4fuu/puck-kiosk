#include "puck/puck_rig.h"
#include "puck/puck_v2.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, const char* file, int line, const std::string& detail = {}) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: check failed: %s", file, line, expression);
    if (!detail.empty()) {
        std::fprintf(stderr, " (%s)", detail.c_str());
    }
    std::fputc('\n', stderr);
}

void check_near(double actual, double expected, double tolerance, const char* expression, const char* file, int line) {
    ++g_checks;
    if (std::fabs(actual - expected) <= tolerance) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: check failed: %s (actual %.9g, expected %.9g)\n", file, line, expression, actual,
                 expected);
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define CHECK_MSG(expression, detail) check((expression), #expression, __FILE__, __LINE__, (detail))
#define CHECK_NEAR(actual, expected, tolerance) check_near((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)

const char* kBaseMeshPath = "assets/cache/3d/base-36354b03.bin";
const char* kArmsMeshPath = "assets/cache/3d/classic/arms-4a1f8a15.bin";
const char* kRigPath = "assets/cache/3d/classic/puck-499552ba.json";

const pk::PuckPart* find_part(const pk::PuckMesh& mesh, const char* name) {
    for (const pk::PuckPart& part : mesh.parts) {
        if (part.name == name) {
            return &part;
        }
    }
    return nullptr;
}

bool load_mesh(const char* path, pk::PuckMesh* mesh) {
    std::string error;
    if (!pk::load_puck_v2(path, mesh, &error)) {
        CHECK_MSG(false, std::string("load ") + path + ": " + error);
        return false;
    }
    return true;
}

void check_part(const pk::PuckMesh& mesh, const char* name, std::size_t vertex_count, std::size_t index_count,
                float min_x, float min_y, float min_z, float max_x, float max_y, float max_z) {
    const pk::PuckPart* part = find_part(mesh, name);
    CHECK_MSG(part != nullptr, std::string("missing part ") + name);
    if (part == nullptr) {
        return;
    }
    CHECK_MSG(part->vertices.size() == vertex_count,
              std::string(name) + " vertices: " + std::to_string(part->vertices.size()));
    CHECK_MSG(part->indices.size() == index_count,
              std::string(name) + " indices: " + std::to_string(part->indices.size()));
    CHECK_NEAR(part->bounds_min.x, min_x, 1e-6f);
    CHECK_NEAR(part->bounds_min.y, min_y, 1e-6f);
    CHECK_NEAR(part->bounds_min.z, min_z, 1e-6f);
    CHECK_NEAR(part->bounds_max.x, max_x, 1e-6f);
    CHECK_NEAR(part->bounds_max.y, max_y, 1e-6f);
    CHECK_NEAR(part->bounds_max.z, max_z, 1e-6f);
}

void test_base_mesh() {
    pk::PuckMesh mesh;
    if (!load_mesh(kBaseMeshPath, &mesh)) {
        return;
    }

    CHECK(mesh.byte_size == 960492);
    CHECK(mesh.parts.size() == 8);
    CHECK(mesh.vertex_count() == 12749);
    CHECK(mesh.index_count() == 72192);

    const char* names[] = {"body", "mouth", "legL", "legR", "nose", "eye", "iris", "lid"};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK_MSG(mesh.parts[i].name == names[i], "part " + std::to_string(i) + " is \"" + mesh.parts[i].name + "\"");
    }

    check_part(mesh, "body", 5550, 31680, -0.4998987019062042f, 0.0f, -0.49998751282691956f, 0.4998832643032074f,
               1.0f, 0.49998751282691956f);
    check_part(mesh, "mouth", 391, 2016, -0.1537340134382248f, 0.5076879858970642f, 0.3979949653148651f,
               0.1537340134382248f, 0.5823120474815369f, 0.497493714094162f);
    check_part(mesh, "legL", 2242, 13056, -1.1812829971313477f, -7.28116512298584f, -1.2645620107650757f,
               1.5108686685562134f, 2.015268564224243f, 2.0482892990112305f);
    check_part(mesh, "legR", 2242, 13056, -1.5108689069747925f, -7.281164646148682f, -1.2645620107650757f,
               1.1812829971313477f, 2.015268325805664f, 2.0482892990112305f);
    check_part(mesh, "nose", 725, 4224, -0.2049599438905716f, -1.0633337497711182f, -0.09323521703481674f,
               0.2049599438905716f, 0.045555561780929565f, 0.5492953062057495f);
    check_part(mesh, "eye", 861, 4560, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f);
    check_part(mesh, "iris", 287, 1320, -0.42430874705314636f, -0.42430874705314636f, 0.9099330306053162f,
               0.42430874705314636f, 0.42430874705314636f, 1.003999948501587f);
    check_part(mesh, "lid", 451, 2280, -1.0467631816864014f, -0.09151352941989899f, -1.0467631816864014f,
               1.0467631816864014f, 1.0499999523162842f, 1.0467631816864014f);

    const pk::PuckPart* body = find_part(mesh, "body");
    if (body != nullptr) {
        CHECK_NEAR(body->vertices[0].position.x, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].position.y, 1.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].position.z, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].normal.x, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].normal.y, 1.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].normal.z, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].tangent.x, -1.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].tangent.y, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].tangent.z, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].tangent.w, 1.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].uv.x, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].uv.y, 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].extra[0], 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].extra[1], 0.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].extra[2], -10.0f, 1e-6f);
        CHECK_NEAR(body->vertices[0].extra[3], 0.0f, 1e-6f);
        CHECK(body->indices[0] == 0);
        CHECK(body->indices[1] == 97);
        CHECK(body->indices[2] == 98);
        CHECK(body->indices[body->indices.size() - 1] == 5452);
    }
}

void test_arms_mesh() {
    pk::PuckMesh mesh;
    if (!load_mesh(kArmsMeshPath, &mesh)) {
        return;
    }

    CHECK(mesh.byte_size == 410548);
    CHECK(mesh.parts.size() == 2);
    CHECK(mesh.vertex_count() == 5388);
    CHECK(mesh.index_count() == 32832);
    CHECK(mesh.parts[0].name == "armL");
    CHECK(mesh.parts[1].name == "armR");

    check_part(mesh, "armL", 2858, 17430, 41.03274154663086f, -501.125f, -28.033742904663086f, 108.00337982177734f,
               -227.21310424804688f, 8.587313652038574f);
    check_part(mesh, "armR", 2530, 15402, 392.5942077636719f, -491.875f, -26.29460334777832f, 453.9133605957031f,
               -258.4127197265625f, 8.340397834777832f);

    const pk::PuckPart* arm = find_part(mesh, "armL");
    if (arm != nullptr) {
        CHECK(arm->indices[0] == 122);
        CHECK(arm->indices[1] == 123);
        CHECK(arm->indices[2] == 780);
        CHECK(arm->indices[arm->indices.size() - 1] == 2830);
    }
}

void test_rig() {
    pk::PuckRig rig;
    std::string error;
    if (!pk::load_puck_rig(kRigPath, &rig, &error)) {
        CHECK_MSG(false, std::string("load rig: ") + error);
        return;
    }

    CHECK(rig.id == "classic");
    CHECK(rig.width == 498);
    CHECK(rig.height == 602);

    CHECK(rig.body.k.size() == 33);
    CHECK(rig.body.cx.size() == 33);
    CHECK_NEAR(rig.body.bottom, 477.0f, 1e-4f);
    CHECK_NEAR(rig.body.height, 411.0f, 1e-4f);
    CHECK_NEAR(rig.body.depth, 1.0f, 1e-4f);
    CHECK_NEAR(rig.body.k[0], 1.173859186732251f, 1e-5f);
    CHECK_NEAR(rig.body.k[32], 0.9618775120291718f, 1e-5f);
    CHECK_NEAR(rig.body.cx[0], 246.51893501375687f, 1e-4f);
    CHECK_NEAR(rig.body.cx[32], 247.32465385303504f, 1e-4f);

    CHECK_NEAR(rig.limbs.leg_l.pivot[0], 186.0148180765115f, 1e-4f);
    CHECK_NEAR(rig.limbs.leg_l.pivot[1], -454.0006148877371f, 1e-4f);
    CHECK_NEAR(rig.limbs.leg_l.pivot[2], 0.0f, 1e-5f);
    CHECK_NEAR(rig.limbs.leg_l.axis[0], -0.012398226754652257f, 1e-6f);
    CHECK_NEAR(rig.limbs.leg_l.axis[1], -0.9999231390328659f, 1e-6f);
    CHECK_NEAR(rig.limbs.leg_l.radius, 18.10316329852705f, 1e-5f);
    CHECK_NEAR(rig.limbs.leg_l.stretch, 0.9189526402178458f, 1e-6f);

    CHECK_NEAR(rig.limbs.leg_r.pivot[0], 317.95240617976623f, 1e-4f);
    CHECK_NEAR(rig.limbs.leg_r.pivot[1], -449.261428282516f, 1e-4f);
    CHECK_NEAR(rig.limbs.leg_r.radius, 17.512924497686132f, 1e-5f);
    CHECK_NEAR(rig.limbs.leg_r.stretch, 0.9372120012370552f, 1e-6f);

    CHECK_NEAR(rig.limbs.arm_l.joints[0][0], 91.01648426846022f, 1e-4f);
    CHECK_NEAR(rig.limbs.arm_l.joints[0][1], -268.0f, 1e-4f);
    CHECK_NEAR(rig.limbs.arm_l.joints[2][0], 65.08290155440415f, 1e-4f);
    CHECK_NEAR(rig.limbs.arm_l.joints[2][1], -430.28f, 1e-4f);
    CHECK_NEAR(rig.limbs.arm_r.joints[0][0], 409.0814797319291f, 1e-4f);
    CHECK_NEAR(rig.limbs.arm_r.joints[2][0], 431.41530054644807f, 1e-4f);
    CHECK_NEAR(rig.limbs.arm_r.joints[2][1], -432.2f, 1e-4f);

    CHECK_NEAR(rig.nose.matrix[0], 78.1263157894737f, 1e-4f);
    CHECK_NEAR(rig.nose.matrix[3], 285.01f, 1e-4f);
    CHECK_NEAR(rig.nose.matrix[7], -178.78f, 1e-4f);
    CHECK_NEAR(rig.nose.matrix[11], 168.96203067673352f, 1e-4f);
    CHECK_NEAR(rig.nose.length, 80.38858879067497f, 1e-5f);

    CHECK(rig.eyes.size() == 2);
    if (rig.eyes.size() == 2) {
        CHECK_NEAR(rig.eyes[0].x, 196.3f, 1e-4f);
        CHECK_NEAR(rig.eyes[0].y, 183.8f, 1e-4f);
        CHECK_NEAR(rig.eyes[0].z, 159.94424666881858f, 1e-4f);
        CHECK_NEAR(rig.eyes[0].r, 27.927000000000003f, 1e-5f);
        CHECK_NEAR(rig.eyes[1].x, 358.3f, 1e-4f);
        CHECK_NEAR(rig.eyes[1].y, 184.3f, 1e-4f);
        CHECK_NEAR(rig.eyes[1].z, 131.40496733962095f, 1e-4f);
        CHECK_NEAR(rig.eyes[1].r, 28.462000000000003f, 1e-5f);
    }

    CHECK_NEAR(rig.mouth.x, 284.0f, 1e-4f);
    CHECK_NEAR(rig.mouth.y, 285.0843610116376f, 1e-4f);
    CHECK_NEAR(rig.mouth.half_width, 64.8f, 1e-4f);
    CHECK_NEAR(rig.mouth.t, 0.4669480267356749f, 1e-6f);
    CHECK_NEAR(rig.mouth.angle, 0.3467366467821357f, 1e-6f);
    CHECK_NEAR(rig.mouth.center, 0.19505258766919836f, 1e-6f);
    CHECK_NEAR(rig.mouth.curve_px, 16.2f, 1e-5f);
    CHECK_NEAR(rig.mouth.curve, -0.03941605839416058f, 1e-6f);

    CHECK_NEAR(rig.ground.x, 248.47088898442826f, 1e-4f);
    CHECK_NEAR(rig.ground.y, 577.0f, 1e-4f);

    CHECK_NEAR(rig.skin_color.r, 80.0f / 255.0f, 1e-6f);
    CHECK_NEAR(rig.skin_color.g, 82.0f / 255.0f, 1e-6f);
    CHECK_NEAR(rig.skin_color.b, 56.0f / 255.0f, 1e-6f);
    CHECK_NEAR(rig.skin_color.a, 1.0f, 1e-6f);
    CHECK_NEAR(rig.sclera_color.r, 227.0f / 255.0f, 1e-6f);
    CHECK_NEAR(rig.sclera_color.g, 222.0f / 255.0f, 1e-6f);
    CHECK_NEAR(rig.sclera_color.b, 199.0f / 255.0f, 1e-6f);
    CHECK_NEAR(rig.sclera_color.a, 1.0f, 1e-6f);

    CHECK_NEAR(rig.material.gloss, 0.1f, 1e-6f);
    CHECK_NEAR(rig.material.height_range[0], -0.8453173329614198f, 1e-6f);
    CHECK_NEAR(rig.material.height_range[1], 0.958781238665943f, 1e-6f);
    CHECK_NEAR(rig.material.rim, 0.6f, 1e-6f);
}

struct ByteWriter {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xffu));
        u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    }

    void u32(std::uint32_t value) {
        u8(static_cast<std::uint8_t>(value & 0xffu));
        u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        u8(static_cast<std::uint8_t>((value >> 16) & 0xffu));
        u8(static_cast<std::uint8_t>((value >> 24) & 0xffu));
    }

    void f32(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void name(const std::string& value) {
        for (std::size_t i = 0; i < 12; ++i) {
            u8(i < value.size() ? static_cast<std::uint8_t>(value[i]) : 0);
        }
    }

    void zeros(std::size_t count) { bytes.insert(bytes.end(), count, 0); }
};

std::vector<std::uint8_t> make_minimal_mesh() {
    ByteWriter writer;
    writer.u8('P');
    writer.u8('U');
    writer.u8('C');
    writer.u8('K');
    writer.u32(2);
    writer.u32(1);
    writer.name("tri");
    writer.u32(3);
    writer.u32(3);

    const float positions[3][3] = {{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    for (const auto& position : positions) {
        writer.f32(position[0]);
        writer.f32(position[1]);
        writer.f32(position[2]);
        writer.f32(0.0f);
        writer.f32(0.0f);
        writer.f32(1.0f);
        writer.f32(1.0f);
        writer.f32(0.0f);
        writer.f32(0.0f);
        writer.f32(1.0f);
        writer.f32(0.0f);
        writer.f32(0.0f);
        writer.zeros(4 * sizeof(float));
    }

    writer.u16(0);
    writer.u16(1);
    writer.u16(2);
    writer.u16(0);
    return writer.bytes;
}

void put_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
    (*bytes)[offset + 0] = static_cast<std::uint8_t>(value & 0xffu);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    (*bytes)[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    (*bytes)[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
}

void put_u16(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value) {
    (*bytes)[offset + 0] = static_cast<std::uint8_t>(value & 0xffu);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
}

void put_f32(std::vector<std::uint8_t>* bytes, std::size_t offset, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(bytes, offset, bits);
}

void expect_mesh_failure(const std::vector<std::uint8_t>& bytes, const std::string& label) {
    pk::PuckMesh mesh;
    std::string error;
    const bool parsed = pk::parse_puck_v2(bytes.data(), bytes.size(), &mesh, &error);
    CHECK_MSG(!parsed, label + " unexpectedly parsed");
    CHECK_MSG(!error.empty(), label + " produced no error");
    CHECK_MSG(mesh.parts.empty(), label + " left partial parts");
}

void test_minimal_mesh_and_malformed() {
    const std::vector<std::uint8_t> valid = make_minimal_mesh();
    CHECK(valid.size() == 232);

    pk::PuckMesh mesh;
    std::string error;
    CHECK_MSG(pk::parse_puck_v2(valid.data(), valid.size(), &mesh, &error), error);
    CHECK(mesh.byte_size == valid.size());
    CHECK(mesh.parts.size() == 1);
    CHECK(mesh.vertex_count() == 3);
    CHECK(mesh.index_count() == 3);
    if (mesh.parts.size() == 1) {
        CHECK(mesh.parts[0].name == "tri");
        CHECK_NEAR(mesh.parts[0].bounds_min.x, -1.0f, 1e-6f);
        CHECK_NEAR(mesh.parts[0].bounds_min.y, 0.0f, 1e-6f);
        CHECK_NEAR(mesh.parts[0].bounds_min.z, 0.0f, 1e-6f);
        CHECK_NEAR(mesh.parts[0].bounds_max.x, 1.0f, 1e-6f);
        CHECK_NEAR(mesh.parts[0].bounds_max.y, 2.0f, 1e-6f);
        CHECK_NEAR(mesh.parts[0].bounds_max.z, 0.0f, 1e-6f);
    }

    ByteWriter empty;
    empty.u8('P');
    empty.u8('U');
    empty.u8('C');
    empty.u8('K');
    empty.u32(2);
    empty.u32(0);
    CHECK(pk::parse_puck_v2(empty.bytes.data(), empty.bytes.size(), &mesh, &error));
    CHECK(mesh.parts.empty());
    CHECK(mesh.byte_size == 12);

    std::vector<std::uint8_t> long_name = valid;
    const char* name = "abcdefghijkl";
    for (std::size_t i = 0; i < 12; ++i) {
        long_name[12 + i] = static_cast<std::uint8_t>(name[i]);
    }
    CHECK(pk::parse_puck_v2(long_name.data(), long_name.size(), &mesh, &error));
    CHECK(mesh.parts.size() == 1 && mesh.parts[0].name == name);

    expect_mesh_failure({}, "empty input");
    expect_mesh_failure(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 11), "short header");

    std::vector<std::uint8_t> bad_magic = valid;
    bad_magic[0] = 'X';
    expect_mesh_failure(bad_magic, "bad magic");

    std::vector<std::uint8_t> bad_version = valid;
    put_u32(&bad_version, 4, 3);
    expect_mesh_failure(bad_version, "bad version");

    std::vector<std::uint8_t> huge_part_count = valid;
    put_u32(&huge_part_count, 8, 0xffffffffu);
    expect_mesh_failure(huge_part_count, "huge part count");

    expect_mesh_failure(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 31), "truncated part header");
    expect_mesh_failure(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 100), "truncated vertices");
    expect_mesh_failure(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 228), "truncated indices");
    expect_mesh_failure(std::vector<std::uint8_t>(valid.begin(), valid.end() - 1), "truncated padding");

    std::vector<std::uint8_t> huge_vertex_count = valid;
    put_u32(&huge_vertex_count, 24, 1000);
    expect_mesh_failure(huge_vertex_count, "huge vertex count");

    std::vector<std::uint8_t> huge_index_count = valid;
    put_u32(&huge_index_count, 28, 1000);
    expect_mesh_failure(huge_index_count, "huge index count");

    std::vector<std::uint8_t> bad_index = valid;
    put_u16(&bad_index, 224, 9);
    expect_mesh_failure(bad_index, "out-of-range index");

    std::vector<std::uint8_t> nan_vertex = valid;
    put_f32(&nan_vertex, 32, std::nanf(""));
    expect_mesh_failure(nan_vertex, "non-finite vertex");

    std::vector<std::uint8_t> trailing = valid;
    trailing.push_back(0);
    expect_mesh_failure(trailing, "trailing data");

    CHECK(!pk::parse_puck_v2(nullptr, 0, &mesh, &error));
    CHECK(!error.empty());
    CHECK(!pk::parse_puck_v2(valid.data(), valid.size(), nullptr, &error));
    CHECK(!error.empty());
    CHECK(!pk::load_puck_v2("assets/cache/does-not-exist.bin", &mesh, &error));
    CHECK(!error.empty());
}

std::string read_text(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void expect_rig_failure(const nlohmann::json& value, const std::string& label) {
    pk::PuckRig rig;
    std::string error;
    const bool parsed = pk::parse_puck_rig(value.dump(), &rig, &error);
    CHECK_MSG(!parsed, label + " unexpectedly parsed");
    CHECK_MSG(!error.empty(), label + " produced no error");
    CHECK_MSG(rig.body.k.empty() && rig.eyes.empty(), label + " left partial rig");
}

void test_malformed_rig() {
    const std::string text = read_text(kRigPath);
    CHECK_MSG(!text.empty(), "could not read rig fixture");
    if (text.empty()) {
        return;
    }

    const nlohmann::json root = nlohmann::json::parse(text);
    pk::PuckRig rig;
    std::string error;

    CHECK(!pk::parse_puck_rig("{", &rig, &error));
    CHECK(!error.empty());
    CHECK(!pk::parse_puck_rig("[]", &rig, &error));
    CHECK(!error.empty());
    CHECK(!pk::parse_puck_rig("{}", &rig, &error));
    CHECK(!error.empty());
    CHECK(!pk::load_puck_rig("assets/cache/does-not-exist.json", &rig, &error));
    CHECK(!error.empty());

    {
        nlohmann::json value = root;
        value.erase("body");
        expect_rig_failure(value, "missing body");
    }
    {
        nlohmann::json value = root;
        value["limbs"].erase("legL");
        expect_rig_failure(value, "missing legL");
    }
    {
        nlohmann::json value = root;
        value.erase("skinColor");
        expect_rig_failure(value, "missing skinColor");
    }
    {
        nlohmann::json value = root;
        value["body"]["k"] = "not an array";
        expect_rig_failure(value, "body.k wrong type");
    }
    {
        nlohmann::json value = root;
        value["body"]["k"] = nlohmann::json::array();
        expect_rig_failure(value, "empty body.k");
    }
    {
        nlohmann::json value = root;
        value["body"]["k"] = nlohmann::json::array({1.0, 2.0});
        expect_rig_failure(value, "body.k/cx length mismatch");
    }
    {
        nlohmann::json value = root;
        value["eyes"] = nlohmann::json::array();
        expect_rig_failure(value, "empty eyes");
    }
    {
        nlohmann::json value = root;
        value["eyes"][0].erase("r");
        expect_rig_failure(value, "eye missing r");
    }
    {
        nlohmann::json value = root;
        value["nose"]["matrix"].erase(value["nose"]["matrix"].begin());
        expect_rig_failure(value, "short nose matrix");
    }
    {
        nlohmann::json value = root;
        value["limbs"]["armL"]["joints"][0].erase(value["limbs"]["armL"]["joints"][0].begin());
        expect_rig_failure(value, "short arm joints row");
    }
    {
        nlohmann::json value = root;
        value["material"]["heightRange"] = nlohmann::json::array({1.0});
        expect_rig_failure(value, "short heightRange");
    }
    {
        nlohmann::json value = root;
        value["skinColor"] = "#xyz";
        expect_rig_failure(value, "bad color");
    }
    {
        nlohmann::json value = root;
        value["skinColor"] = "505238";
        expect_rig_failure(value, "color without hash");
    }
    {
        nlohmann::json value = root;
        value["id"] = 3;
        expect_rig_failure(value, "non-string id");
    }
    {
        nlohmann::json value = root;
        value["width"] = -1;
        expect_rig_failure(value, "negative width");
    }
    {
        nlohmann::json value = root;
        value["width"] = 1.5;
        expect_rig_failure(value, "fractional width");
    }

    {
        nlohmann::json value = root;
        value.erase("id");
        value.erase("width");
        value.erase("height");
        value["futureField"] = true;
        CHECK_MSG(pk::parse_puck_rig(value.dump(), &rig, &error), error);
        CHECK(rig.id.empty());
        CHECK(rig.width == 0);
        CHECK(rig.height == 0);
        CHECK(rig.eyes.size() == 2);
    }
    {
        nlohmann::json value = root;
        value["skinColor"] = "#ABC";
        CHECK_MSG(pk::parse_puck_rig(value.dump(), &rig, &error), error);
        CHECK_NEAR(rig.skin_color.r, 170.0f / 255.0f, 1e-6f);
        CHECK_NEAR(rig.skin_color.g, 187.0f / 255.0f, 1e-6f);
        CHECK_NEAR(rig.skin_color.b, 204.0f / 255.0f, 1e-6f);
    }
    {
        nlohmann::json value = root;
        value["scleraColor"] = "#E3DEC780";
        CHECK_MSG(pk::parse_puck_rig(value.dump(), &rig, &error), error);
        CHECK_NEAR(rig.sclera_color.r, 227.0f / 255.0f, 1e-6f);
        CHECK_NEAR(rig.sclera_color.g, 222.0f / 255.0f, 1e-6f);
        CHECK_NEAR(rig.sclera_color.b, 199.0f / 255.0f, 1e-6f);
        CHECK_NEAR(rig.sclera_color.a, 128.0f / 255.0f, 1e-6f);
    }
}

struct TestCase {
    const char* name;
    void (*function)();
};

}  // namespace

int main() {
    const TestCase tests[] = {
        {"base mesh", test_base_mesh},
        {"arms mesh", test_arms_mesh},
        {"rig", test_rig},
        {"minimal mesh and malformed input", test_minimal_mesh_and_malformed},
        {"malformed rig", test_malformed_rig},
    };

    std::printf("[==========] puck-tests\n");
    for (const TestCase& test : tests) {
        const int failures_before = g_failures;
        std::printf("[ RUN      ] %s\n", test.name);
        test.function();
        if (g_failures == failures_before) {
            std::printf("[       OK ] %s\n", test.name);
        } else {
            std::printf("[   FAILED ] %s\n", test.name);
        }
    }

    if (g_failures == 0) {
        std::printf("[  PASSED  ] %d checks\n", g_checks);
        return 0;
    }
    std::printf("[  FAILED  ] %d of %d checks\n", g_failures, g_checks);
    return 1;
}
