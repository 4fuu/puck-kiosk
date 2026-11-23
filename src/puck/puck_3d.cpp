#include "puck/puck_3d.h"

#include "core/log.h"
#include "puck/puck_3d_pose.h"
#include "puck/puck_rig.h"
#include "puck/puck_v2.h"

#include <GLES3/gl3.h>
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pk {
namespace {

constexpr const char* kBaseMeshPath = "assets/cache/3d/base-36354b03.bin";
constexpr const char* kArmsMeshPath = "assets/cache/3d/classic/arms-4a1f8a15.bin";
constexpr const char* kRigPath = "assets/cache/3d/classic/puck-499552ba.json";
constexpr const char* kAlbedoPath = "assets/cache/3d/classic/albedo-09254c33.webp";
constexpr const char* kNormalPath = "assets/cache/3d/classic/normal-539ef9bd.webp";
constexpr const char* kImagePath = "assets/cache/3d/classic/image-983fb864.webp";
constexpr int kTextureSize = 512;
constexpr float kPi = 3.14159265358979323846f;

constexpr const char* kPartVertexShader = R"(#version 300 es
precision highp float;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec4 aRig;

uniform mat4 uProjection;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform vec4 uMouth;
uniform vec3 uMouthShape;

out vec2 vUV;
out vec3 vNormal;
out vec3 vRestNormal;
out vec4 vTangent;

vec3 moveMouth(vec3 p) {
    if (uMouth.w < 0.5) {
        return p;
    }
    float open = uMouthShape.x;
    float spread = max(0.0, uMouthShape.y);
    float roundness = max(0.0, -uMouthShape.y);
    float press = uMouthShape.z;
    float half_width = uMouth.z;
    float profile = max(0.0, 1.0 - aRig.z * aRig.z);
    vec3 delta = vec3(0.0);
    delta.y -= open * 0.9 * half_width * (1.0 - 0.45 * spread + 0.1 * roundness) * aRig.x *
               pow(profile, 0.6);
    delta.y += open * 0.22 * half_width * (1.0 + 0.8 * roundness) * aRig.y * pow(profile, 0.7);
    delta.z -= open * 0.2 * half_width * aRig.x * profile;
    delta.x += (p.x - uMouth.x) * (0.24 * spread - 0.42 * roundness + 0.06 * press) * aRig.w;
    delta.z += (0.2 * roundness - 0.06 * press) * half_width * aRig.w * profile;
    return p + delta;
}

void main() {
    vec3 p = moveMouth(aPosition);
    vUV = aUV;
    vNormal = uNormalMatrix * aNormal;
    vRestNormal = aNormal;
    vTangent = vec4(mat3(uModel) * aTangent.xyz, aTangent.w);
    gl_Position = uProjection * uModel * vec4(p, 1.0);
}
)";

constexpr const char* kPartFragmentShader = R"(#version 300 es
precision highp float;

uniform sampler2D uAlbedo;
uniform sampler2D uNormalMap;
uniform sampler2D uImage;
uniform vec3 uLight;
uniform float uGloss;
uniform float uRim;
uniform float uProp;
uniform float uPropOpacity;
uniform float uSpotlight;
uniform vec3 uSpotColor;

in vec2 vUV;
in vec3 vNormal;
in vec3 vRestNormal;
in vec4 vTangent;
out vec4 outColor;

float shade(vec3 n) {
    return 0.45 + 0.55 * max(dot(n, uLight), 0.0);
}

float specular(vec3 n) {
    vec3 h = normalize(uLight + vec3(0.0, 0.0, 1.0));
    return pow(max(dot(n, h), 0.0), 48.0);
}

void main() {
    vec3 N = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    if (uProp > 0.5) {
        // Arms are painted in the base image; relight that color by how far
        // the surface turned from its rest normal.
        vec3 image = texture(uImage, vUV).rgb;
        vec3 rest = normalize(vRestNormal);
        rest.z = abs(rest.z);
        float relight = min(shade(N) / shade(rest), 1.15);
        vec3 lit = image * relight +
                   uSpotlight * uSpotColor * image * (0.12 + 0.4 * max(N.y, 0.0));
        outColor = vec4(clamp(lit, 0.0, 1.0) * uPropOpacity, uPropOpacity);
        return;
    }
    vec3 T = normalize(vTangent.xyz - N * dot(N, vTangent.xyz));
    vec3 B = cross(N, T) * vTangent.w;
    float blur = gl_FrontFacing ? 0.0 : 3.0;
    vec4 normalSample = texture(uNormalMap, vUV, blur);
    vec3 relief = normalSample.xyz * 2.0 - 1.0;
    vec3 n = normalize(T * relief.x + B * relief.y + N * relief.z);
    vec4 albedo = texture(uAlbedo, vUV, blur);
    float glow = 1.0 - normalSample.a;
    float rim = uRim * pow(1.0 - clamp(N.z, 0.0, 1.0), 3.0);
    float through = min(1.0, 1.5 * (1.0 - albedo.a));
    float lit = mix(shade(n) + rim, 1.0, max(glow, 0.5 * through));
    vec3 rgb = albedo.rgb * lit + uGloss * specular(n) * (1.0 - glow);
    rgb += uSpotlight * uSpotColor * albedo.rgb * (0.12 + 0.4 * max(n.y, 0.0));
    outColor = vec4(clamp(rgb, 0.0, 1.0) * albedo.a, albedo.a);
}
)";

constexpr const char* kEyeVertexShader = R"(#version 300 es
precision highp float;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;

uniform mat4 uProjection;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;

out vec3 vLocal;
out vec3 vNormal;

void main() {
    vLocal = aPosition;
    vNormal = uNormalMatrix * aNormal;
    gl_Position = uProjection * uModel * vec4(aPosition, 1.0);
}
)";

constexpr const char* kEyeFragmentShader = R"(#version 300 es
precision highp float;

uniform vec3 uLight;
uniform vec3 uSclera;
uniform vec3 uSkin;
uniform float uKind;

in vec3 vLocal;
in vec3 vNormal;
out vec4 outColor;

void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, uLight), 0.0);
    vec3 h = normalize(uLight + vec3(0.0, 0.0, 1.0));
    float rim = smoothstep(0.0, 0.45, n.z);
    float highlight = pow(max(dot(n, h), 0.0), 90.0);
    vec3 rgb;
    if (uKind > 1.5) {
        float crease = 1.0 - 0.55 * smoothstep(0.25, 0.0, normalize(vLocal).y + 0.087);
        rgb = uSkin * (0.5 + 0.6 * diffuse) * crease * mix(0.7, 1.0, rim);
    } else if (uKind > 0.5) {
        float c = normalize(vLocal).z;
        rgb = mix(vec3(0.05, 0.043, 0.039), vec3(0.165, 0.15, 0.133), smoothstep(0.905, 1.0, c));
        rgb = rgb * mix(0.55, 1.0, rim) + vec3(0.85) * highlight;
    } else {
        rgb = uSclera * (0.62 + 0.45 * diffuse) * mix(0.55, 1.0, rim) + vec3(0.85) * highlight;
    }
    outColor = vec4(min(rgb, vec3(1.0)), 1.0);
}
)";

constexpr const char* kSpotlightVertexShader = R"(#version 300 es
precision highp float;

uniform mat4 uProjection;
uniform vec3 uCenter;
uniform vec2 uHalfSize;

out vec2 vPlane;

void main() {
    vec2 corner = vec2(gl_VertexID == 1 || gl_VertexID >= 4 ? 1.0 : -1.0,
                       gl_VertexID == 2 || gl_VertexID == 3 || gl_VertexID == 4 ? 1.0 : -1.0);
    vPlane = corner * uHalfSize;
    gl_Position = uProjection * vec4(uCenter + vec3(vPlane, 0.0), 1.0);
}
)";

constexpr const char* kSpotlightFragmentShader = R"(#version 300 es
precision highp float;

uniform vec2 uHalfSize;
uniform vec3 uPool;
uniform vec3 uSpotColor;
uniform float uSpotlight;
uniform vec2 uViewport;

in vec2 vPlane;
out vec4 outColor;

void main() {
    vec2 p = vPlane;
    float down = clamp((uHalfSize.y - p.y) / (uHalfSize.y - uPool.z), 0.0, 1.0);
    float halfWidth = uHalfSize.x * mix(0.2, 0.95, down);
    float edge = 1.0 - smoothstep(0.45, 1.0, abs(p.x) / halfWidth);
    float along = smoothstep(0.0, 0.45, down) * (1.0 - 0.3 * down);
    along *= smoothstep(uPool.z - 0.4 * uPool.y, uPool.z + 0.6 * uPool.y, p.y);
    float beam = 0.2 * edge * along;
    vec2 q = vec2(p.x, p.y - uPool.z) / uPool.xy;
    float r = dot(q, q);
    float oval = 0.5 * exp(-3.0 * r) * (1.0 - smoothstep(0.6, 1.0, r));
    float a = uSpotlight * (1.0 - (1.0 - beam) * (1.0 - oval));
    vec2 view = gl_FragCoord.xy / uViewport;
    a *= smoothstep(0.0, 0.15, view.x) * smoothstep(0.0, 0.15, 1.0 - view.x);
    a *= smoothstep(0.0, 0.15, 1.0 - view.y) * smoothstep(0.0, 0.05, view.y);
    outColor = vec4(uSpotColor * a, a);
}
)";

constexpr const char* kCompositeVertexShader = R"(#version 300 es
precision highp float;

out vec2 vUV;

void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

constexpr const char* kCompositeFragmentShader = R"(#version 300 es
precision highp float;

uniform sampler2D uScene;
uniform float uSaturation;
uniform float uBrightness;

in vec2 vUV;
out vec4 outColor;

void main() {
    vec4 scene = texture(uScene, vUV);
    float luma = dot(scene.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 rgb = mix(vec3(luma), scene.rgb, uSaturation) * uBrightness;
    outColor = vec4(rgb, scene.a);
}
)";

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Mat4 {
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

Vec3 normalize(Vec3 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-9f) {
        return Vec3{0.0f, 0.0f, 1.0f};
    }
    return Vec3{v.x / length, v.y / length, v.z / length};
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                value += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
            result.m[column * 4 + row] = value;
        }
    }
    return result;
}

Mat4 translate(float x, float y, float z) {
    Mat4 result;
    result.m[12] = x;
    result.m[13] = y;
    result.m[14] = z;
    return result;
}

Mat4 scale(float x, float y, float z) {
    Mat4 result;
    result.m[0] = x;
    result.m[5] = y;
    result.m[10] = z;
    return result;
}

Mat4 rotate_x(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 result;
    result.m[5] = c;
    result.m[6] = s;
    result.m[9] = -s;
    result.m[10] = c;
    return result;
}

Mat4 rotate_y(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 result;
    result.m[0] = c;
    result.m[2] = -s;
    result.m[8] = s;
    result.m[10] = c;
    return result;
}

Mat4 rotate_z(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 result;
    result.m[0] = c;
    result.m[1] = s;
    result.m[4] = -s;
    result.m[5] = c;
    return result;
}

Mat4 orthographic(float left, float right, float bottom, float top, float near, float far) {
    Mat4 result;
    result.m[0] = 2.0f / (right - left);
    result.m[5] = 2.0f / (top - bottom);
    result.m[10] = -2.0f / (far - near);
    result.m[12] = -(right + left) / (right - left);
    result.m[13] = -(top + bottom) / (top - bottom);
    result.m[14] = -(far + near) / (far - near);
    return result;
}

void normal_matrix(const Mat4& matrix, float out[9]) {
    const float t = matrix.m[0];
    const float n = matrix.m[1];
    const float r = matrix.m[2];
    const float i = matrix.m[4];
    const float a = matrix.m[5];
    const float o = matrix.m[6];
    const float s = matrix.m[8];
    const float c = matrix.m[9];
    const float l = matrix.m[10];
    const float cof00 = a * l - o * c;
    const float cof10 = -(i * l - o * s);
    const float cof20 = i * c - a * s;
    const float cof01 = -(n * l - r * c);
    const float cof11 = t * l - r * s;
    const float cof21 = -(t * c - n * s);
    const float cof02 = n * o - r * a;
    const float cof12 = -(t * o - r * i);
    const float cof22 = t * a - n * i;
    const float determinant = t * cof00 + n * cof10 + r * cof20;
    const float inverse = std::abs(determinant) > 1e-12f ? 1.0f / determinant : 0.0f;
    out[0] = cof00 * inverse;
    out[1] = cof10 * inverse;
    out[2] = cof20 * inverse;
    out[3] = cof01 * inverse;
    out[4] = cof11 * inverse;
    out[5] = cof21 * inverse;
    out[6] = cof02 * inverse;
    out[7] = cof12 * inverse;
    out[8] = cof22 * inverse;
}

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct LoadedPart {
    std::string name;
    std::vector<PuckVertex> vertices;
    std::vector<std::uint16_t> indices;
};

struct LoadedScene {
    PuckRig rig;
    std::vector<LoadedPart> base_parts;
    std::vector<LoadedPart> arm_parts;
    Image albedo;
    Image normal;
    Image image;
};

bool read_file(const std::string& path, std::vector<std::uint8_t>* data, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) {
            *error = "cannot open " + path;
        }
        return false;
    }
    data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (data->empty()) {
        if (error) {
            *error = path + " is empty";
        }
        return false;
    }
    return true;
}

bool decode_webp(const std::string& path, int max_size, Image* image, std::string* error) {
    std::vector<std::uint8_t> encoded;
    if (!read_file(path, &encoded, error)) {
        return false;
    }
    int width = 0;
    int height = 0;
    if (WebPGetInfo(encoded.data(), encoded.size(), &width, &height) == 0 || width <= 0 ||
        height <= 0) {
        if (error) {
            *error = "cannot decode WebP header: " + path;
        }
        return false;
    }
    std::uint8_t* decoded = WebPDecodeRGBA(encoded.data(), encoded.size(), &width, &height);
    if (!decoded) {
        if (error) {
            *error = "cannot decode WebP pixels: " + path;
        }
        return false;
    }

    const int scale = std::max(1, (std::max(width, height) + max_size - 1) / max_size);
    const int target_width = std::max(1, width / scale);
    const int target_height = std::max(1, height / scale);
    image->width = target_width;
    image->height = target_height;
    image->rgba.assign(static_cast<std::size_t>(target_width) * target_height * 4, 0);

    const auto source = [&](int x, int y, int channel) {
        return static_cast<std::uint32_t>(
            decoded[(static_cast<std::size_t>(y) * width + x) * 4 + channel]);
    };
    for (int y = 0; y < target_height; ++y) {
        const int source_y0 = y * height / target_height;
        const int source_y1 = std::max(source_y0 + 1, (y + 1) * height / target_height);
        for (int x = 0; x < target_width; ++x) {
            const int source_x0 = x * width / target_width;
            const int source_x1 = std::max(source_x0 + 1, (x + 1) * width / target_width);
            std::uint32_t sums[4] = {0, 0, 0, 0};
            std::uint32_t count = 0;
            for (int sy = source_y0; sy < source_y1; ++sy) {
                for (int sx = source_x0; sx < source_x1; ++sx) {
                    for (int channel = 0; channel < 4; ++channel) {
                        sums[channel] += source(sx, sy, channel);
                    }
                    ++count;
                }
            }
            const std::size_t out = (static_cast<std::size_t>(y) * target_width + x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                image->rgba[out + channel] =
                    static_cast<std::uint8_t>((sums[channel] + count / 2) / count);
            }
        }
    }
    WebPFree(decoded);
    return true;
}

const LoadedPart* find_part(const std::vector<LoadedPart>& parts, const char* name) {
    for (const LoadedPart& part : parts) {
        if (part.name == name) {
            return &part;
        }
    }
    return nullptr;
}

bool load_scene(LoadedScene* scene, std::string* error) {
    PuckMesh base;
    PuckMesh arms;
    std::string detail;
    if (!load_puck_v2(kBaseMeshPath, &base, &detail)) {
        if (error) {
            *error = std::string("base mesh: ") + detail;
        }
        return false;
    }
    if (!load_puck_v2(kArmsMeshPath, &arms, &detail)) {
        if (error) {
            *error = std::string("arms mesh: ") + detail;
        }
        return false;
    }
    if (!load_puck_rig(kRigPath, &scene->rig, &detail)) {
        if (error) {
            *error = std::string("rig: ") + detail;
        }
        return false;
    }
    if (scene->rig.width <= 0 || scene->rig.height <= 0 || scene->rig.eyes.empty() ||
        scene->rig.mouth.half_width <= 0.0f) {
        if (error) {
            *error = "rig has no usable face";
        }
        return false;
    }

    for (const PuckPart& part : base.parts) {
        LoadedPart loaded;
        loaded.name = part.name;
        loaded.indices = part.indices;
        if (part.name == "eye" || part.name == "iris" || part.name == "lid") {
            loaded.vertices = part.vertices;
        } else if (is_posed_part(part.name)) {
            if (!pose_puck_part(part, scene->rig, &loaded.vertices, &detail)) {
                if (error) {
                    *error = "pose " + part.name + ": " + detail;
                }
                return false;
            }
        } else {
            continue;
        }
        scene->base_parts.push_back(std::move(loaded));
    }
    for (const PuckPart& part : arms.parts) {
        LoadedPart loaded;
        loaded.name = part.name;
        loaded.vertices = part.vertices;
        loaded.indices = part.indices;
        scene->arm_parts.push_back(std::move(loaded));
    }

    const char* required_base[] = {"body", "mouth", "legL", "legR", "nose", "eye", "iris", "lid"};
    for (const char* name : required_base) {
        if (!find_part(scene->base_parts, name)) {
            if (error) {
                *error = std::string("base mesh is missing part ") + name;
            }
            return false;
        }
    }
    if (!find_part(scene->arm_parts, "armL") || !find_part(scene->arm_parts, "armR")) {
        if (error) {
            *error = "arms mesh is missing armL or armR";
        }
        return false;
    }

    if (!decode_webp(kAlbedoPath, kTextureSize, &scene->albedo, &detail)) {
        if (error) {
            *error = "albedo texture: " + detail;
        }
        return false;
    }
    if (!decode_webp(kNormalPath, kTextureSize, &scene->normal, &detail)) {
        if (error) {
            *error = "normal texture: " + detail;
        }
        return false;
    }
    if (!decode_webp(kImagePath, 1024, &scene->image, &detail)) {
        if (error) {
            *error = "image texture: " + detail;
        }
        return false;
    }
    return true;
}

bool load_scene_threaded(LoadedScene* scene, std::string* error) {
    LoadedScene result;
    std::string detail;
    bool ok = false;
    try {
        std::thread worker([&]() {
            try {
                ok = load_scene(&result, &detail);
            } catch (const std::exception& exception) {
                detail = exception.what();
                ok = false;
            } catch (...) {
                detail = "asset loading failed";
                ok = false;
            }
        });
        worker.join();
    } catch (const std::exception&) {
        result = LoadedScene{};
        detail.clear();
        ok = load_scene(&result, &detail);
    }
    if (!ok) {
        if (error) {
            *error = detail;
        }
        return false;
    }
    *scene = std::move(result);
    return true;
}

class GlProgram {
public:
    ~GlProgram() { destroy(); }
    GlProgram() = default;
    GlProgram(const GlProgram&) = delete;
    GlProgram& operator=(const GlProgram&) = delete;

    bool build(const char* vertex_source, const char* fragment_source, std::string* error) {
        destroy();
        const auto compile = [&](GLenum type, const char* source) -> GLuint {
            const GLuint shader = glCreateShader(type);
            if (!shader) {
                return 0;
            }
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::string log(static_cast<std::size_t>(std::max(1, length)), '\0');
                glGetShaderInfoLog(shader, length, nullptr, log.data());
                if (error) {
                    *error = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") +
                             " shader: " + log;
                }
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        };
        const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
        if (!vertex) {
            return false;
        }
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
        if (!fragment) {
            glDeleteShader(vertex);
            return false;
        }
        program_ = glCreateProgram();
        if (!program_) {
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            if (error) {
                *error = "glCreateProgram failed";
            }
            return false;
        }
        glAttachShader(program_, vertex);
        glAttachShader(program_, fragment);
        glLinkProgram(program_);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(1, length)), '\0');
            glGetProgramInfoLog(program_, length, nullptr, log.data());
            if (error) {
                *error = "link: " + log;
            }
            destroy();
            return false;
        }
        return true;
    }

    void destroy() {
        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        locations_.clear();
    }

    void use() const { glUseProgram(program_); }

    GLint location(const char* name) {
        const auto found = locations_.find(name);
        if (found != locations_.end()) {
            return found->second;
        }
        const GLint location = glGetUniformLocation(program_, name);
        locations_.emplace(name, location);
        return location;
    }

    void set_float(const char* name, float value) { glUniform1f(location(name), value); }
    void set_int(const char* name, int value) { glUniform1i(location(name), value); }
    void set_vec2(const char* name, float x, float y) { glUniform2f(location(name), x, y); }
    void set_vec3(const char* name, const Vec3& value) {
        glUniform3f(location(name), value.x, value.y, value.z);
    }
    void set_vec3(const char* name, float x, float y, float z) {
        glUniform3f(location(name), x, y, z);
    }
    void set_vec4(const char* name, float x, float y, float z, float w) {
        glUniform4f(location(name), x, y, z, w);
    }
    void set_mat4(const char* name, const Mat4& value) {
        glUniformMatrix4fv(location(name), 1, GL_FALSE, value.m);
    }
    void set_mat3(const char* name, const float value[9]) {
        glUniformMatrix3fv(location(name), 1, GL_FALSE, value);
    }

private:
    GLuint program_ = 0;
    std::unordered_map<std::string, GLint> locations_;
};

struct GlMesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLsizei count = 0;
};

void destroy_mesh(GlMesh* mesh) {
    if (mesh->ibo) {
        glDeleteBuffers(1, &mesh->ibo);
    }
    if (mesh->vbo) {
        glDeleteBuffers(1, &mesh->vbo);
    }
    if (mesh->vao) {
        glDeleteVertexArrays(1, &mesh->vao);
    }
    *mesh = GlMesh{};
}

bool upload_mesh(const LoadedPart& part, GlMesh* mesh, std::string* error) {
    destroy_mesh(mesh);
    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ibo);
    if (!mesh->vao || !mesh->vbo || !mesh->ibo) {
        if (error) {
            *error = "cannot allocate a vertex buffer for " + part.name;
        }
        destroy_mesh(mesh);
        return false;
    }
    glBindVertexArray(mesh->vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(part.vertices.size() * sizeof(PuckVertex)),
                 part.vertices.data(), GL_STATIC_DRAW);
    const auto attribute = [&](GLuint index, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, sizeof(PuckVertex),
                              reinterpret_cast<const void*>(offset));
    };
    attribute(0, 3, offsetof(PuckVertex, position));
    attribute(1, 3, offsetof(PuckVertex, normal));
    attribute(2, 4, offsetof(PuckVertex, tangent));
    attribute(3, 2, offsetof(PuckVertex, uv));
    attribute(4, 4, offsetof(PuckVertex, extra));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(part.indices.size() * sizeof(std::uint16_t)),
                 part.indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    mesh->count = static_cast<GLsizei>(part.indices.size());
    if (glGetError() != GL_NO_ERROR) {
        if (error) {
            *error = "cannot upload " + part.name;
        }
        destroy_mesh(mesh);
        return false;
    }
    return true;
}

bool upload_texture(const Image& image, GLuint* texture, std::string* error) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        if (error) {
            *error = "texture has no pixels";
        }
        return false;
    }
    glGenTextures(1, texture);
    if (!*texture) {
        if (error) {
            *error = "cannot allocate a texture";
        }
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, *texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, image.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        if (error) {
            *error = "cannot upload a texture";
        }
        glDeleteTextures(1, texture);
        *texture = 0;
        return false;
    }
    return true;
}

struct Offscreen {
    GLuint framebuffer = 0;
    GLuint color = 0;
    GLuint depth = 0;
    int width = 0;
    int height = 0;
};

void destroy_offscreen(Offscreen* target) {
    if (target->depth) {
        glDeleteRenderbuffers(1, &target->depth);
    }
    if (target->color) {
        glDeleteTextures(1, &target->color);
    }
    if (target->framebuffer) {
        glDeleteFramebuffers(1, &target->framebuffer);
    }
    *target = Offscreen{};
}

bool ensure_offscreen(Offscreen* target, int width, int height, std::string* error) {
    if (target->framebuffer && target->width == width && target->height == height) {
        return true;
    }
    destroy_offscreen(target);
    glGenFramebuffers(1, &target->framebuffer);
    glGenTextures(1, &target->color);
    glGenRenderbuffers(1, &target->depth);
    if (!target->framebuffer || !target->color || !target->depth) {
        if (error) {
            *error = "cannot allocate the offscreen target";
        }
        destroy_offscreen(target);
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, target->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindRenderbuffer(GL_RENDERBUFFER, target->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              target->depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) {
            *error = "offscreen target is incomplete";
        }
        destroy_offscreen(target);
        return false;
    }
    target->width = width;
    target->height = height;
    log::info("puck-3d: %dx%d offscreen target (%.1f MB)", width, height,
              static_cast<double>(width) * height * 6.0 / 1e6);
    return true;
}

struct Animation {
    float last_time = -1.0f;
    float gaze_x = 0.0f;
    float gaze_y = 0.0f;
    float lids = 0.0f;
    float mouth = 0.0f;
    float spotlight = 0.0f;
    float filter = 0.0f;
    float blink = 0.0f;
};

}  // namespace

struct Puck3D::Impl {
    ~Impl() { destroy(); }

    bool initialize(std::string* error);
    void draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds);
    void destroy();

    bool ready = false;
    bool failed = false;
    PuckRig rig;
    GlProgram part_program;
    GlProgram eye_program;
    GlProgram spotlight_program;
    GlProgram composite_program;
    GlMesh body;
    GlMesh mouth;
    GlMesh leg_l;
    GlMesh leg_r;
    GlMesh nose;
    GlMesh arm_l;
    GlMesh arm_r;
    GlMesh eye;
    GlMesh iris;
    GlMesh lid;
    GLuint albedo = 0;
    GLuint normal = 0;
    GLuint image = 0;
    GLuint empty_vao = 0;
    Offscreen offscreen;
    Animation animation;
    std::size_t gpu_buffer_bytes = 0;
    std::size_t texture_bytes = 0;
};

namespace {

bool upload_part(const std::vector<LoadedPart>& parts, const char* name, GlMesh* mesh,
                 std::string* error) {
    const LoadedPart* part = find_part(parts, name);
    if (!part) {
        if (error) {
            *error = std::string("missing part ") + name;
        }
        return false;
    }
    return upload_mesh(*part, mesh, error);
}

Mat4 body_model(const PuckRig& rig, float squash, float yaw, float pitch, float roll, float slide,
                float lift) {
    const Vec3 ground{rig.ground.x, -rig.ground.y, 0.0f};
    const float inverse_sqrt = 1.0f / std::sqrt(std::max(1e-6f, squash));
    Mat4 model = translate(slide, lift, 0.0f);
    model = multiply(model, translate(ground.x, ground.y, ground.z));
    model = multiply(model, rotate_y(yaw));
    model = multiply(model, rotate_x(pitch));
    model = multiply(model, rotate_z(roll));
    model = multiply(model, scale(inverse_sqrt, squash, inverse_sqrt));
    model = multiply(model, translate(-ground.x, -ground.y, -ground.z));
    return model;
}

Mat4 projection_for(const PuckRig& rig, int width, int height) {
    const float fit = std::min(static_cast<float>(width) / static_cast<float>(rig.width),
                               static_cast<float>(height) / static_cast<float>(rig.height));
    const float half_width = static_cast<float>(width) / fit * 0.5f;
    const float half_height = static_cast<float>(height) / fit * 0.5f;
    const float center_x = static_cast<float>(rig.width) * 0.5f;
    const float center_y = -static_cast<float>(rig.height) * 0.5f;
    return orthographic(center_x - half_width, center_x + half_width, center_y - half_height,
                        center_y + half_height, -2000.0f, 2000.0f);
}

Mat4 gaze_rotation(float gaze_x, float gaze_y) {
    const float magnitude = std::hypot(gaze_x, gaze_y);
    const float tilt = std::asin((0.42f / 1.07f) * std::min(1.0f, magnitude));
    const float x = magnitude < 1e-4f ? 0.0f : std::sin(tilt) * gaze_x / magnitude;
    const float y = magnitude < 1e-4f ? 0.0f : -std::sin(tilt) * gaze_y / magnitude;
    const float pitch = -std::asin(std::max(-1.0f, std::min(1.0f, y)));
    const float yaw = std::asin(std::max(-1.0f, std::min(1.0f, x / std::cos(pitch))));
    return multiply(rotate_y(yaw), rotate_x(pitch));
}

const Vec3 kLight = normalize(Vec3{-0.45f, 0.6f, 0.65f});
const Vec3 kSpotColor{1.0f, 0.78f, 0.42f};

}  // namespace

bool Puck3D::Impl::initialize(std::string* error) {
    if (ready) {
        return true;
    }
    const auto fail = [&](const std::string& message) {
        if (error) {
            *error = "puck-3d: " + message;
        }
        destroy();
        return false;
    };

    const GLubyte* version = glGetString(GL_VERSION);
    if (!version) {
        return fail("no current GLES context");
    }
    const char* version_text = reinterpret_cast<const char*>(version);
    if (!std::strstr(version_text, "OpenGL ES 3")) {
        return fail(std::string("GLES 3.0 is required (found ") + version_text + ")");
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    const auto started = std::chrono::steady_clock::now();
    LoadedScene scene;
    std::string detail;
    if (!load_scene_threaded(&scene, &detail)) {
        return fail(detail);
    }
    const auto loaded = std::chrono::steady_clock::now();

    if (!part_program.build(kPartVertexShader, kPartFragmentShader, &detail) ||
        !eye_program.build(kEyeVertexShader, kEyeFragmentShader, &detail) ||
        !spotlight_program.build(kSpotlightVertexShader, kSpotlightFragmentShader, &detail) ||
        !composite_program.build(kCompositeVertexShader, kCompositeFragmentShader, &detail)) {
        return fail(detail);
    }
    if (!upload_texture(scene.albedo, &albedo, &detail) ||
        !upload_texture(scene.normal, &normal, &detail) ||
        !upload_texture(scene.image, &image, &detail)) {
        return fail(detail);
    }
    const std::size_t texture_pixels =
        static_cast<std::size_t>(scene.albedo.width) * scene.albedo.height * 4 +
        static_cast<std::size_t>(scene.normal.width) * scene.normal.height * 4 +
        static_cast<std::size_t>(scene.image.width) * scene.image.height * 4;
    texture_bytes = texture_pixels;
    gpu_buffer_bytes = 0;
    for (const LoadedPart& part : scene.base_parts) {
        gpu_buffer_bytes += part.vertices.size() * sizeof(PuckVertex) + part.indices.size() * 2;
    }
    for (const LoadedPart& part : scene.arm_parts) {
        gpu_buffer_bytes += part.vertices.size() * sizeof(PuckVertex) + part.indices.size() * 2;
    }
    if (!upload_part(scene.base_parts, "body", &body, &detail) ||
        !upload_part(scene.base_parts, "mouth", &mouth, &detail) ||
        !upload_part(scene.base_parts, "legL", &leg_l, &detail) ||
        !upload_part(scene.base_parts, "legR", &leg_r, &detail) ||
        !upload_part(scene.base_parts, "nose", &nose, &detail) ||
        !upload_part(scene.base_parts, "eye", &eye, &detail) ||
        !upload_part(scene.base_parts, "iris", &iris, &detail) ||
        !upload_part(scene.base_parts, "lid", &lid, &detail) ||
        !upload_part(scene.arm_parts, "armL", &arm_l, &detail) ||
        !upload_part(scene.arm_parts, "armR", &arm_r, &detail)) {
        return fail(detail);
    }
    glGenVertexArrays(1, &empty_vao);
    if (!empty_vao) {
        return fail("cannot allocate the empty vertex array");
    }
    rig = std::move(scene.rig);
    ready = true;
    const auto uploaded = std::chrono::steady_clock::now();
    const auto milliseconds = [](auto from, auto to) {
        return std::chrono::duration<float, std::milli>(to - from).count();
    };
    log::info("puck-3d: %s %dx%d, %zu base parts, %zu arm parts, %.1f MB buffers, %.1f MB textures",
              rig.id.c_str(), rig.width, rig.height, scene.base_parts.size(),
              scene.arm_parts.size(), static_cast<double>(gpu_buffer_bytes) / 1e6,
              static_cast<double>(texture_bytes) / 1e6);
    log::info("puck-3d: loaded in %.0f ms, uploaded in %.0f ms", milliseconds(started, loaded),
              milliseconds(loaded, uploaded));
    return true;
}

void Puck3D::Impl::destroy() {
    if (empty_vao) {
        glDeleteVertexArrays(1, &empty_vao);
        empty_vao = 0;
    }
    destroy_mesh(&body);
    destroy_mesh(&mouth);
    destroy_mesh(&leg_l);
    destroy_mesh(&leg_r);
    destroy_mesh(&nose);
    destroy_mesh(&arm_l);
    destroy_mesh(&arm_r);
    destroy_mesh(&eye);
    destroy_mesh(&iris);
    destroy_mesh(&lid);
    if (albedo) {
        glDeleteTextures(1, &albedo);
        albedo = 0;
    }
    if (normal) {
        glDeleteTextures(1, &normal);
        normal = 0;
    }
    if (image) {
        glDeleteTextures(1, &image);
        image = 0;
    }
    destroy_offscreen(&offscreen);
    part_program.destroy();
    eye_program.destroy();
    spotlight_program.destroy();
    composite_program.destroy();
    ready = false;
}

namespace {

void update_animation(Animation* animation, const PuckVisual& visual, float time_seconds) {
    const bool first = animation->last_time < 0.0f;
    const float dt = first ? 0.0f
                           : std::min(0.1f, std::max(0.0f, time_seconds - animation->last_time));
    animation->last_time = time_seconds;

    float target_x = 0.0f;
    float target_y = 0.0f;
    if (visual.not_listening) {
        target_x = 0.0f;
        target_y = 0.0f;
    } else if (visual.thinking) {
        target_x = 0.5f * std::sin(time_seconds * 0.9f) + 0.08f;
        target_y = -0.65f + 0.08f * std::sin(time_seconds * 1.7f);
    } else if (visual.speaking || visual.listening) {
        target_y = 0.1f;
    } else {
        target_x = 0.45f * std::sin(time_seconds * 0.37f) +
                   0.15f * std::sin(time_seconds * 0.91f + 1.7f);
        target_y = 0.12f * std::sin(time_seconds * 0.53f + 0.6f);
    }
    if (first) {
        animation->gaze_x = target_x;
        animation->gaze_y = target_y;
    } else {
        const float follow = 1.0f - std::exp(-dt * 18.0f);
        animation->gaze_x += (target_x - animation->gaze_x) * follow;
        animation->gaze_y += (target_y - animation->gaze_y) * follow;
    }

    const float blink_phase = std::fmod(time_seconds + 1.3f, 4.6f);
    float blink = 0.0f;
    if (blink_phase < 0.07f) {
        blink = blink_phase / 0.07f;
    } else if (blink_phase < 0.18f) {
        blink = 1.0f - (blink_phase - 0.07f) / 0.11f;
    }
    const float lid_target = visual.not_listening ? 1.0f : 0.0f;
    if (first) {
        animation->lids = lid_target;
    } else {
        animation->lids += (lid_target - animation->lids) * (1.0f - std::exp(-dt * 14.0f));
    }
    animation->blink = std::max(blink, animation->lids);

    float mouth_target = std::max(0.0f, std::min(1.0f, visual.mouth));
    if (visual.speaking) {
        const float speech = 0.16f + 0.34f * (0.5f + 0.5f * std::sin(time_seconds * 8.7f)) *
                                          (0.65f + 0.35f * std::sin(time_seconds * 3.1f + 1.2f));
        mouth_target = std::max(mouth_target, speech);
    }
    if (first) {
        animation->mouth = mouth_target;
    } else {
        const float rate = mouth_target > animation->mouth ? 18.0f : 24.0f;
        animation->mouth += (mouth_target - animation->mouth) * (1.0f - std::exp(-dt * rate));
    }
    animation->mouth = std::max(0.0f, std::min(1.0f, animation->mouth));

    const float spotlight_target = visual.speaking ? 1.0f : 0.0f;
    const float filter_target = visual.not_listening ? 1.0f : 0.0f;
    if (first) {
        animation->spotlight = spotlight_target;
        animation->filter = filter_target;
    } else {
        animation->spotlight +=
            (spotlight_target - animation->spotlight) * (1.0f - std::exp(-dt * 4.0f));
        animation->filter += (filter_target - animation->filter) * (1.0f - std::exp(-dt * 4.0f));
    }
}

void draw_mesh(const GlMesh& mesh) {
    if (mesh.count <= 0) {
        return;
    }
    glBindVertexArray(mesh.vao);
    glDrawElements(GL_TRIANGLES, mesh.count, GL_UNSIGNED_SHORT, nullptr);
}

}  // namespace

void Puck3D::Impl::draw(Canvas& canvas, Rect bounds, const PuckVisual& visual,
                        float time_seconds) {
    if (!ready || failed || bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return;
    }
    update_animation(&animation, visual, std::max(0.0f, time_seconds));

    GLint surface[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, surface);
    if (surface[2] <= 0 || surface[3] <= 0) {
        return;
    }
    const int x0 = std::max(0, std::min(surface[2], static_cast<int>(std::floor(bounds.x))));
    const int y0 = std::max(0, std::min(surface[3], static_cast<int>(std::floor(bounds.y))));
    const int x1 = std::max(x0, std::min(surface[2], static_cast<int>(std::ceil(bounds.x + bounds.w))));
    const int y1 = std::max(y0, std::min(surface[3], static_cast<int>(std::ceil(bounds.y + bounds.h))));
    const int width = x1 - x0;
    const int height = y1 - y0;
    if (width <= 0 || height <= 0) {
        return;
    }
    const int viewport_x = surface[0] + x0;
    const int viewport_y = surface[1] + (surface[3] - y1);

    GLint surface_framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &surface_framebuffer);
    GLfloat clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);

    // Submit the 2D background first so the 3D layer lands over it and the rest
    // of the UI is queued on top. NanoVG's flush needs the surface bound.
    glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer);
    canvas.flush();

    std::string detail;
    if (!ensure_offscreen(&offscreen, width, height, &detail)) {
        log::error("puck-3d: %s; keeping the 2D layer", detail.c_str());
        failed = true;
        glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, offscreen.framebuffer);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const Mat4 projection = projection_for(rig, width, height);
    const float breathing = visual.speaking
                                ? 0.0f
                                : std::sin(2.0f * kPi * time_seconds / 3.2f) *
                                      std::sin(2.0f * kPi * time_seconds / 8.0f);
    const float squash = 1.0f - 0.018f * breathing;
    const float roll = 0.03f * std::sin(2.0f * kPi * 0.8f * time_seconds) * animation.spotlight;
    const Mat4 body_transform = body_model(rig, squash, 0.0f, 0.0f, roll, 0.0f, 0.0f);
    float body_normal[9];
    normal_matrix(body_transform, body_normal);

    part_program.use();
    part_program.set_mat4("uProjection", projection);
    part_program.set_vec3("uLight", kLight);
    part_program.set_float("uGloss", rig.material.gloss);
    part_program.set_float("uRim", rig.material.rim);
    part_program.set_int("uAlbedo", 0);
    part_program.set_int("uNormalMap", 1);
    part_program.set_int("uImage", 2);
    part_program.set_float("uProp", 0.0f);
    part_program.set_float("uPropOpacity", 1.0f);
    part_program.set_float("uSpotlight", animation.spotlight);
    part_program.set_vec3("uSpotColor", kSpotColor);
    const float mouth_open = std::max(0.0f, std::min(1.0f, (animation.mouth - 0.02f) / 0.5f)) * 0.7f;
    part_program.set_vec3("uMouthShape", mouth_open, 0.0f, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, albedo);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, normal);

    if (animation.spotlight >= 0.002f) {
        const float ground_y = -rig.ground.y;
        const float top = 0.7f * static_cast<float>(rig.height);
        const float pool_width = 0.8f * static_cast<float>(rig.width);
        const float pool_height = 0.16f * static_cast<float>(rig.width);
        const float pool_bottom = ground_y - pool_height;
        const float center_y = (top + pool_bottom) * 0.5f;
        spotlight_program.use();
        spotlight_program.set_mat4("uProjection", projection);
        spotlight_program.set_vec3("uCenter", rig.ground.x, center_y,
                                   -0.6f * static_cast<float>(rig.height));
        spotlight_program.set_vec2("uHalfSize", 0.9f * static_cast<float>(rig.width),
                                   (top - pool_bottom) * 0.5f);
        spotlight_program.set_vec3("uPool", pool_width, pool_height, ground_y - center_y);
        spotlight_program.set_vec3("uSpotColor", kSpotColor);
        spotlight_program.set_float("uSpotlight", animation.spotlight);
        spotlight_program.set_vec2("uViewport", static_cast<float>(width),
                                   static_cast<float>(height));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(empty_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        part_program.use();
    }

    const auto draw_part = [&](const GlMesh& mesh, bool mouth_rig) {
        part_program.set_mat4("uModel", body_transform);
        part_program.set_mat3("uNormalMatrix", body_normal);
        part_program.set_vec4("uMouth", mouth_rig ? rig.mouth.x : 0.0f,
                              mouth_rig ? -rig.mouth.y : 0.0f,
                              mouth_rig ? rig.mouth.half_width : 1.0f, mouth_rig ? 1.0f : 0.0f);
        draw_mesh(mesh);
    };
    draw_part(mouth, true);
    draw_part(leg_l, false);
    draw_part(leg_r, false);
    draw_part(nose, false);

    part_program.set_float("uProp", 1.0f);
    part_program.set_vec4("uMouth", 0.0f, 0.0f, 1.0f, 0.0f);
    part_program.set_mat4("uModel", body_transform);
    part_program.set_mat3("uNormalMatrix", body_normal);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, image);
    draw_mesh(arm_l);
    draw_mesh(arm_r);
    part_program.set_float("uProp", 0.0f);

    draw_part(body, true);

    eye_program.use();
    eye_program.set_mat4("uProjection", projection);
    eye_program.set_vec3("uLight", kLight);
    eye_program.set_vec3("uSclera", rig.sclera_color.r, rig.sclera_color.g, rig.sclera_color.b);
    eye_program.set_vec3("uSkin", rig.skin_color.r, rig.skin_color.g, rig.skin_color.b);
    const Mat4 gaze = gaze_rotation(animation.gaze_x, animation.gaze_y);
    const Mat4 lid_turn = rotate_x((-95.0f + 185.0f * animation.blink) * kPi / 180.0f);
    for (const PuckRigEye& eye_rig : rig.eyes) {
        const Mat4 center = multiply(body_transform, translate(eye_rig.x, -eye_rig.y, eye_rig.z));
        const Mat4 scaled = multiply(center, scale(eye_rig.r, eye_rig.r, eye_rig.r));
        float ball_normal[9];
        float iris_normal[9];
        float lid_normal[9];
        normal_matrix(center, ball_normal);
        normal_matrix(multiply(center, gaze), iris_normal);
        normal_matrix(multiply(center, lid_turn), lid_normal);
        eye_program.set_mat4("uModel", scaled);
        eye_program.set_mat3("uNormalMatrix", ball_normal);
        eye_program.set_float("uKind", 0.0f);
        draw_mesh(eye);
        eye_program.set_mat4("uModel", multiply(scaled, gaze));
        eye_program.set_mat3("uNormalMatrix", iris_normal);
        eye_program.set_float("uKind", 1.0f);
        draw_mesh(iris);
        eye_program.set_mat4("uModel", multiply(scaled, lid_turn));
        eye_program.set_mat3("uNormalMatrix", lid_normal);
        eye_program.set_float("uKind", 2.0f);
        draw_mesh(lid);
    }
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer);
    glViewport(viewport_x, viewport_y, width, height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    composite_program.use();
    composite_program.set_int("uScene", 0);
    composite_program.set_float("uSaturation", 1.0f + (0.15f - 1.0f) * animation.filter);
    composite_program.set_float("uBrightness", 1.0f + (0.6f - 1.0f) * animation.filter);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, offscreen.color);
    glBindVertexArray(empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glViewport(surface[0], surface[1], surface[2], surface[3]);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
}

Puck3D::Puck3D() : impl_(std::make_unique<Impl>()) {}
Puck3D::~Puck3D() = default;

bool Puck3D::initialize(std::string* error) {
    return impl_->initialize(error);
}

void Puck3D::draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) {
    impl_->draw(canvas, bounds, visual, time_seconds);
}

}  // namespace pk
