#include "puck/puck_rig.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace pk {
namespace {

using Json = nlohmann::json;

bool fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

const Json* member(const Json& object, const char* key, const std::string& path, std::string* error) {
    if (!object.is_object()) {
        fail(error, path + " must be an object");
        return nullptr;
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        fail(error, path + "." + key + " is missing");
        return nullptr;
    }
    return &*it;
}

const Json* object_member(const Json& parent, const char* key, const std::string& path, std::string* error) {
    const Json* value = member(parent, key, path, error);
    if (value == nullptr) {
        return nullptr;
    }
    if (!value->is_object()) {
        fail(error, path + "." + key + " must be an object");
        return nullptr;
    }
    return value;
}

bool number_member(const Json& object, const char* key, const std::string& path, float* out, std::string* error) {
    const Json* value = member(object, key, path, error);
    if (value == nullptr) {
        return false;
    }
    if (!value->is_number()) {
        return fail(error, path + "." + key + " must be a number");
    }
    const double number = value->get<double>();
    if (!std::isfinite(number)) {
        return fail(error, path + "." + key + " must be finite");
    }
    *out = static_cast<float>(number);
    return true;
}

bool number_array_member(const Json& object, const char* key, const std::string& path, std::vector<float>* out,
                         std::string* error) {
    const Json* value = member(object, key, path, error);
    if (value == nullptr) {
        return false;
    }
    if (!value->is_array()) {
        return fail(error, path + "." + key + " must be an array");
    }

    std::vector<float> numbers;
    numbers.reserve(value->size());
    for (std::size_t i = 0; i < value->size(); ++i) {
        const Json& item = (*value)[i];
        const std::string item_path = path + "." + key + "[" + std::to_string(i) + "]";
        if (!item.is_number()) {
            return fail(error, item_path + " must be a number");
        }
        const double number = item.get<double>();
        if (!std::isfinite(number)) {
            return fail(error, item_path + " must be finite");
        }
        numbers.push_back(static_cast<float>(number));
    }
    *out = std::move(numbers);
    return true;
}

bool fixed_numbers_member(const Json& object, const char* key, const std::string& path, float* out,
                          std::size_t count, std::string* error) {
    const Json* value = member(object, key, path, error);
    if (value == nullptr) {
        return false;
    }
    if (!value->is_array() || value->size() != count) {
        return fail(error, path + "." + key + " must be an array of " + std::to_string(count) + " numbers");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Json& item = (*value)[i];
        const std::string item_path = path + "." + key + "[" + std::to_string(i) + "]";
        if (!item.is_number()) {
            return fail(error, item_path + " must be a number");
        }
        const double number = item.get<double>();
        if (!std::isfinite(number)) {
            return fail(error, item_path + " must be finite");
        }
        out[i] = static_cast<float>(number);
    }
    return true;
}

bool matrix3_member(const Json& object, const char* key, const std::string& path, float out[3][3],
                    std::string* error) {
    const Json* value = member(object, key, path, error);
    if (value == nullptr) {
        return false;
    }
    if (!value->is_array() || value->size() != 3) {
        return fail(error, path + "." + key + " must be an array of 3 rows");
    }
    for (std::size_t row = 0; row < 3; ++row) {
        const Json& row_value = (*value)[row];
        const std::string row_path = path + "." + key + "[" + std::to_string(row) + "]";
        if (!row_value.is_array() || row_value.size() != 3) {
            return fail(error, row_path + " must be an array of 3 numbers");
        }
        for (std::size_t column = 0; column < 3; ++column) {
            const Json& item = row_value[column];
            const std::string item_path = row_path + "[" + std::to_string(column) + "]";
            if (!item.is_number()) {
                return fail(error, item_path + " must be a number");
            }
            const double number = item.get<double>();
            if (!std::isfinite(number)) {
                return fail(error, item_path + " must be finite");
            }
            out[row][column] = static_cast<float>(number);
        }
    }
    return true;
}

bool string_member(const Json& object, const char* key, const std::string& path, std::string* out,
                   std::string* error) {
    const Json* value = member(object, key, path, error);
    if (value == nullptr) {
        return false;
    }
    if (!value->is_string()) {
        return fail(error, path + "." + key + " must be a string");
    }
    *out = value->get<std::string>();
    return true;
}

bool optional_string_member(const Json& object, const char* key, const std::string& path, std::string* out,
                            std::string* error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_string()) {
        return fail(error, path + "." + key + " must be a string");
    }
    *out = it->get<std::string>();
    return true;
}

bool optional_int_member(const Json& object, const char* key, const std::string& path, int* out,
                         std::string* error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_number()) {
        return fail(error, path + "." + key + " must be an integer");
    }
    const double number = it->get<double>();
    if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 1000000.0) {
        return fail(error, path + "." + key + " must be a non-negative integer");
    }
    *out = static_cast<int>(number);
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool color_member(const Json& object, const char* key, const std::string& path, Color* out, std::string* error) {
    std::string text;
    if (!string_member(object, key, path, &text, error)) {
        return false;
    }
    const std::string item_path = path + "." + key;
    if (text.empty() || text[0] != '#') {
        return fail(error, item_path + " must be a #rgb, #rrggbb, or #rrggbbaa color");
    }

    const std::string hex = text.substr(1);
    if (hex.size() != 3 && hex.size() != 6 && hex.size() != 8) {
        return fail(error, item_path + " must be a #rgb, #rrggbb, or #rrggbbaa color");
    }
    for (char c : hex) {
        if (hex_value(c) < 0) {
            return fail(error, item_path + " contains a non-hex character");
        }
    }

    if (hex.size() == 3) {
        out->r = static_cast<float>(hex_value(hex[0]) * 17) / 255.0f;
        out->g = static_cast<float>(hex_value(hex[1]) * 17) / 255.0f;
        out->b = static_cast<float>(hex_value(hex[2]) * 17) / 255.0f;
        out->a = 1.0f;
        return true;
    }

    const auto channel = [&hex](std::size_t offset) {
        return static_cast<float>(hex_value(hex[offset]) * 16 + hex_value(hex[offset + 1])) / 255.0f;
    };
    out->r = channel(0);
    out->g = channel(2);
    out->b = channel(4);
    out->a = hex.size() == 8 ? channel(6) : 1.0f;
    return true;
}

bool parse_body(const Json& root, PuckRigBody* body, std::string* error) {
    const Json* object = object_member(root, "body", "rig", error);
    if (object == nullptr) {
        return false;
    }
    const std::string path = "rig.body";
    if (!number_member(*object, "bottom", path, &body->bottom, error) ||
        !number_member(*object, "height", path, &body->height, error) ||
        !number_member(*object, "depth", path, &body->depth, error) ||
        !number_array_member(*object, "k", path, &body->k, error) ||
        !number_array_member(*object, "cx", path, &body->cx, error)) {
        return false;
    }
    if (body->k.empty()) {
        return fail(error, "rig.body.k must not be empty");
    }
    if (body->k.size() != body->cx.size()) {
        return fail(error, "rig.body.k and rig.body.cx must have the same length");
    }
    return true;
}

bool parse_leg(const Json& limbs, const char* key, const std::string& path, PuckRigLeg* leg, std::string* error) {
    const Json* object = object_member(limbs, key, path, error);
    if (object == nullptr) {
        return false;
    }
    const std::string leg_path = path + "." + key;
    return fixed_numbers_member(*object, "pivot", leg_path, leg->pivot, 3, error) &&
           fixed_numbers_member(*object, "axis", leg_path, leg->axis, 3, error) &&
           number_member(*object, "radius", leg_path, &leg->radius, error) &&
           number_member(*object, "stretch", leg_path, &leg->stretch, error);
}

bool parse_arm(const Json& limbs, const char* key, const std::string& path, PuckRigArm* arm, std::string* error) {
    const Json* object = object_member(limbs, key, path, error);
    if (object == nullptr) {
        return false;
    }
    return matrix3_member(*object, "joints", path + "." + key, arm->joints, error);
}

bool parse_limbs(const Json& root, PuckRigLimbs* limbs, std::string* error) {
    const Json* object = object_member(root, "limbs", "rig", error);
    if (object == nullptr) {
        return false;
    }
    const std::string path = "rig.limbs";
    return parse_leg(*object, "legL", path, &limbs->leg_l, error) &&
           parse_leg(*object, "legR", path, &limbs->leg_r, error) &&
           parse_arm(*object, "armL", path, &limbs->arm_l, error) &&
           parse_arm(*object, "armR", path, &limbs->arm_r, error);
}

bool parse_nose(const Json& root, PuckRigNose* nose, std::string* error) {
    const Json* object = object_member(root, "nose", "rig", error);
    if (object == nullptr) {
        return false;
    }
    const std::string path = "rig.nose";
    return fixed_numbers_member(*object, "matrix", path, nose->matrix, 12, error) &&
           number_member(*object, "length", path, &nose->length, error);
}

bool parse_eyes(const Json& root, std::vector<PuckRigEye>* eyes, std::string* error) {
    const Json* value = member(root, "eyes", "rig", error);
    if (value == nullptr) {
        return false;
    }
    if (!value->is_array()) {
        return fail(error, "rig.eyes must be an array");
    }
    if (value->empty()) {
        return fail(error, "rig.eyes must not be empty");
    }

    std::vector<PuckRigEye> parsed;
    parsed.reserve(value->size());
    for (std::size_t i = 0; i < value->size(); ++i) {
        const Json& item = (*value)[i];
        const std::string path = "rig.eyes[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return fail(error, path + " must be an object");
        }
        PuckRigEye eye;
        if (!number_member(item, "x", path, &eye.x, error) ||
            !number_member(item, "y", path, &eye.y, error) ||
            !number_member(item, "z", path, &eye.z, error) ||
            !number_member(item, "r", path, &eye.r, error)) {
            return false;
        }
        parsed.push_back(eye);
    }
    *eyes = std::move(parsed);
    return true;
}

bool parse_mouth(const Json& root, PuckRigMouth* mouth, std::string* error) {
    const Json* object = object_member(root, "mouth", "rig", error);
    if (object == nullptr) {
        return false;
    }
    const std::string path = "rig.mouth";
    return number_member(*object, "x", path, &mouth->x, error) &&
           number_member(*object, "y", path, &mouth->y, error) &&
           number_member(*object, "halfWidth", path, &mouth->half_width, error) &&
           number_member(*object, "t", path, &mouth->t, error) &&
           number_member(*object, "angle", path, &mouth->angle, error) &&
           number_member(*object, "center", path, &mouth->center, error) &&
           number_member(*object, "curvePx", path, &mouth->curve_px, error) &&
           number_member(*object, "curve", path, &mouth->curve, error);
}

bool parse_ground(const Json& root, PuckRigGround* ground, std::string* error) {
    const Json* object = object_member(root, "ground", "rig", error);
    if (object == nullptr) {
        return false;
    }
    const std::string path = "rig.ground";
    return number_member(*object, "x", path, &ground->x, error) &&
           number_member(*object, "y", path, &ground->y, error);
}

bool parse_material(const Json& root, PuckRigMaterial* material, std::string* error) {
    const Json* object = object_member(root, "material", "rig", error);
    if (object == nullptr) {
        return false;
    }
    const std::string path = "rig.material";
    return number_member(*object, "gloss", path, &material->gloss, error) &&
           fixed_numbers_member(*object, "heightRange", path, material->height_range, 2, error) &&
           number_member(*object, "rim", path, &material->rim, error);
}

bool parse_rig_json(const Json& root, PuckRig* rig, std::string* error) {
    if (!root.is_object()) {
        return fail(error, "PUCK rig must be a JSON object");
    }
    if (!optional_string_member(root, "id", "rig", &rig->id, error) ||
        !optional_int_member(root, "width", "rig", &rig->width, error) ||
        !optional_int_member(root, "height", "rig", &rig->height, error) ||
        !parse_body(root, &rig->body, error) ||
        !parse_limbs(root, &rig->limbs, error) ||
        !parse_nose(root, &rig->nose, error) ||
        !parse_eyes(root, &rig->eyes, error) ||
        !parse_mouth(root, &rig->mouth, error) ||
        !parse_ground(root, &rig->ground, error) ||
        !color_member(root, "skinColor", "rig", &rig->skin_color, error) ||
        !color_member(root, "scleraColor", "rig", &rig->sclera_color, error) ||
        !parse_material(root, &rig->material, error)) {
        return false;
    }
    return true;
}

}  // namespace

bool parse_puck_rig(const std::string& json, PuckRig* rig, std::string* error) {
    if (rig == nullptr) {
        return fail(error, "PUCK rig output is null");
    }
    *rig = PuckRig{};

    try {
        const Json root = Json::parse(json, nullptr, false);
        if (root.is_discarded()) {
            return fail(error, "invalid PUCK rig JSON");
        }
        PuckRig parsed;
        if (!parse_rig_json(root, &parsed, error)) {
            return false;
        }
        *rig = std::move(parsed);
        return true;
    } catch (const std::bad_alloc&) {
        *rig = PuckRig{};
        return fail(error, "out of memory while parsing PUCK rig");
    }
}

bool load_puck_rig(const std::string& path, PuckRig* rig, std::string* error) {
    if (rig == nullptr) {
        return fail(error, "PUCK rig output is null");
    }
    *rig = PuckRig{};

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return fail(error, "cannot open PUCK rig file \"" + path + "\"");
    }
    const std::streamoff end = file.tellg();
    if (end < 0) {
        return fail(error, "cannot determine the size of PUCK rig file \"" + path + "\"");
    }
    if (static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::uint64_t>(end) > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return fail(error, "PUCK rig file \"" + path + "\" is too large");
    }

    const std::size_t size = static_cast<std::size_t>(end);
    try {
        std::string text(size, '\0');
        if (size != 0) {
            file.seekg(0, std::ios::beg);
            if (!file) {
                return fail(error, "cannot seek PUCK rig file \"" + path + "\"");
            }
            file.read(text.data(), static_cast<std::streamsize>(size));
            if (file.gcount() != static_cast<std::streamsize>(size)) {
                return fail(error, "failed to read PUCK rig file \"" + path + "\"");
            }
        }
        return parse_puck_rig(text, rig, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "out of memory while reading PUCK rig file \"" + path + "\"");
    }
}

}  // namespace pk
