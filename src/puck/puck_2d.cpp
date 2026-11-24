#include "puck/puck_2d.h"

#include "core/log.h"

#include <nlohmann/json.hpp>
#include <png.h>
#include <webp/decode.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pk {
namespace {

constexpr const char* kSpritePngPath = "assets/cache/avatars/classic-9012f611.png";
constexpr const char* kSpriteWebpPath = "assets/cache/3d/classic/image-983fb864.webp";
constexpr const char* kFaceJsonPath = "assets/cache/3d/classic/puck-499552ba.json";

// The web client gets a 2D face descriptor from the server. Classic's values
// are: left eye (196.3, 183.8, r 26.1), right eye (358.3, 184.3, r 26.6),
// mouth (284, 294, halfWidth 64.8, sag 0). The rig JSON has the same eye
// centers and mouth x/halfWidth; these two factors reproduce the descriptor
// from the rig so the overlay sits exactly on the rendered face.
constexpr float kEyeOverlayScale = 0.9346f;
constexpr float kMouthBaselineOffset = 0.55f;

// The interaction uses fractions of the image height, so the 2D renderer
// needs the same reference frame as the rig.
constexpr float kPuckAspect = 498.0f / 602.0f;
constexpr float kPuckGroundY = 577.0f / 602.0f;
constexpr float kPuckEyeX = 277.3f / 498.0f;
constexpr float kPuckEyeY = 184.05f / 602.0f;
// The web's `U = 0.45` tanh tip limit; a bigger shear is not representable
// with the Canvas API's axis-aligned image draws.
constexpr float kMaxLean = 0.45f;
constexpr int kBandCount = 64;

// The web mouth overlay is authored in a canonical 38 px half-width space.
constexpr float kMouthCanonicalHalfWidth = 38.0f;
constexpr float kMouthCanonicalLine = 1.1f;
constexpr float kMouthOpenUpper = 13.0f;
constexpr float kMouthOpenLower = 54.0f;

struct Eye {
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
};

struct Mouth {
    float x = 0.0f;
    float y = 0.0f;
    float half_width = 0.0f;
    float sag = 0.0f;
};

struct Sprite {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Face {
    int width = 0;
    int height = 0;
    Eye left_eye;
    Eye right_eye;
    Mouth mouth;
    Color skin = hex(0x505238);
    Color sclera = hex(0xe3dec7);
};

struct Loaded {
    bool sprite_ok = false;
    bool face_ok = false;
    std::string error;
    Sprite sprite;
    Face face;
};

// The breathing transform is applied to the sprite and the face together so
// the overlay stays on the rendered features.
struct Pose {
    Rect image;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float breathe_x = 1.0f;
    float breathe_y = 1.0f;
    float lift = 0.0f;
    float lean = 0.0f;
    float pivot_y = 0.0f;

    float base_x(float face_x) const {
        const float local = image.x + face_x * scale_x;
        return origin_x + (local - origin_x) * breathe_x;
    }

    float base_y(float face_y) const {
        const float local = image.y + face_y * scale_y;
        return origin_y + (local - origin_y) * breathe_y + lift;
    }

    // A horizontal shear around the ground reads as a lean. The Canvas seam
    // has no rotated-image draw, so Puck2D draws the sprite in bands.
    float shear(float y) const { return std::tan(lean) * (y - pivot_y); }

    float x(float face_x, float face_y) const { return base_x(face_x) + shear(base_y(face_y)); }
    float y(float face_y) const { return base_y(face_y); }

    float rx(float radius) const { return radius * scale_x * breathe_x; }
    float ry(float radius) const { return radius * scale_y * breathe_y; }
};

Color fade(Color color, float alpha) {
    color.a *= alpha;
    return color;
}

Color shade(Color color, float amount) {
    const float target = amount > 0.0f ? 1.0f : 0.0f;
    const float blend = std::min(1.0f, std::abs(amount));
    color.r += (target - color.r) * blend;
    color.g += (target - color.g) * blend;
    color.b += (target - color.b) * blend;
    return color;
}

bool read_file(const std::string& path, std::string* contents, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) {
            *error = "cannot open " + path;
        }
        return false;
    }
    contents->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        if (error) {
            *error = "cannot read " + path;
        }
        return false;
    }
    return true;
}

bool decode_png(const std::string& path, Sprite* sprite, std::string* error) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        if (error) {
            *error = "PNG: " + std::string(image.message);
        }
        png_image_free(&image);
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    sprite->width = static_cast<int>(image.width);
    sprite->height = static_cast<int>(image.height);
    sprite->rgba.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, sprite->rgba.data(), 0, nullptr)) {
        if (error) {
            *error = "PNG: " + std::string(image.message);
        }
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);
    return true;
}

bool decode_webp(const std::string& path, Sprite* sprite, std::string* error) {
    std::string bytes;
    if (!read_file(path, &bytes, error)) {
        return false;
    }
    int width = 0;
    int height = 0;
    std::uint8_t* rgba = WebPDecodeRGBA(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                        bytes.size(), &width, &height);
    if (!rgba || width <= 0 || height <= 0) {
        if (error) {
            *error = "WebP: decode failed";
        }
        WebPFree(rgba);
        return false;
    }
    sprite->width = width;
    sprite->height = height;
    const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    sprite->rgba.assign(rgba, rgba + size);
    WebPFree(rgba);
    return true;
}

bool load_sprite(Sprite* sprite, std::string* error) {
    std::string png_error;
    if (std::filesystem::exists(kSpritePngPath) && decode_png(kSpritePngPath, sprite, &png_error)) {
        return true;
    }
    if (!std::filesystem::exists(kSpritePngPath)) {
        png_error = std::string("missing ") + kSpritePngPath;
    }

    std::string webp_error;
    if (std::filesystem::exists(kSpriteWebpPath) && decode_webp(kSpriteWebpPath, sprite, &webp_error)) {
        log::warn("2D Puck: %s; using the opaque WebP sprite", png_error.c_str());
        return true;
    }
    if (!std::filesystem::exists(kSpriteWebpPath)) {
        webp_error = std::string("missing ") + kSpriteWebpPath;
    }
    if (error) {
        *error = png_error + "; " + webp_error;
    }
    return false;
}

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

Color json_color(const nlohmann::json& json, const char* key, Color fallback) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
        return fallback;
    }
    const std::string value = it->get<std::string>();
    if (value.size() != 7 || value.front() != '#') {
        return fallback;
    }
    std::uint32_t rgb = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const int nibble = hex_nibble(value[index]);
        if (nibble < 0) {
            return fallback;
        }
        rgb = (rgb << 4u) | static_cast<std::uint32_t>(nibble);
    }
    return hex(rgb);
}

bool load_face(const Sprite& sprite, Face* face, std::string* error) {
    std::string text;
    if (!read_file(kFaceJsonPath, &text, error)) {
        return false;
    }

    try {
        const nlohmann::json json = nlohmann::json::parse(text);
        face->width = json.value("width", sprite.width);
        face->height = json.value("height", sprite.height);

        const auto& eyes = json.at("eyes");
        if (!eyes.is_array() || eyes.size() < 2) {
            throw std::runtime_error("eyes must contain two entries");
        }
        face->left_eye.x = eyes.at(0).at("x").get<float>();
        face->left_eye.y = eyes.at(0).at("y").get<float>();
        face->left_eye.r = eyes.at(0).at("r").get<float>() * kEyeOverlayScale;
        face->right_eye.x = eyes.at(1).at("x").get<float>();
        face->right_eye.y = eyes.at(1).at("y").get<float>();
        face->right_eye.r = eyes.at(1).at("r").get<float>() * kEyeOverlayScale;

        const auto& mouth = json.at("mouth");
        face->mouth.x = mouth.at("x").get<float>();
        face->mouth.y = mouth.at("y").get<float>() +
                        mouth.value("curvePx", 0.0f) * kMouthBaselineOffset;
        face->mouth.half_width = mouth.at("halfWidth").get<float>();
        // The web's classic 2D descriptor is a straight baseline (sag 0); the
        // rig's curve field is a 3D lip parameter, not the 2D overlay sag.
        face->mouth.sag = 0.0f;
        face->skin = json_color(json, "skinColor", face->skin);
        face->sclera = json_color(json, "scleraColor", face->sclera);
    } catch (const std::exception& exception) {
        if (error) {
            *error = std::string("face JSON: ") + exception.what();
        }
        return false;
    }

    if (face->width <= 0 || face->height <= 0 || face->mouth.half_width <= 0.0f ||
        face->left_eye.r <= 0.0f || face->right_eye.r <= 0.0f) {
        if (error) {
            *error = "face JSON: invalid geometry";
        }
        return false;
    }
    return true;
}

Loaded load_assets() {
    Loaded loaded;
    if (!load_sprite(&loaded.sprite, &loaded.error)) {
        return loaded;
    }
    loaded.sprite_ok = true;
    loaded.face_ok = load_face(loaded.sprite, &loaded.face, &loaded.error);
    return loaded;
}

}  // namespace

struct Puck2D::Assets {
    Sprite sprite;
    Face face;
    bool face_ok = false;
};

Puck2D::Puck2D() = default;

Puck2D::~Puck2D() {
    release_image();
}

bool Puck2D::initialize(std::string* error) {
    if (assets_) {
        return true;
    }

    Loaded loaded;
    std::thread worker([&loaded] { loaded = load_assets(); });
    worker.join();

    if (!loaded.sprite_ok) {
        if (error) {
            *error = loaded.error;
        }
        log::warn("2D Puck assets unavailable (%s); using the placeholder", loaded.error.c_str());
        return true;
    }

    auto assets = std::make_unique<Assets>();
    assets->sprite = std::move(loaded.sprite);
    assets->face = loaded.face;
    assets->face_ok = loaded.face_ok;
    log::info("2D Puck sprite: %dx%d%s", assets->sprite.width, assets->sprite.height,
              assets->face_ok ? " with the face overlay" : " without face data");
    if (!assets->face_ok) {
        log::warn("2D Puck face data unavailable (%s)", loaded.error.c_str());
    }
    assets_ = std::move(assets);
    return true;
}

void Puck2D::draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) {
    if (!assets_) {
        draw_placeholder(canvas, bounds, visual, time_seconds);
        return;
    }

    if (image_ == kInvalidImage && !image_failed_) {
        canvas_ = &canvas;
        image_ = canvas.create_image(assets_->sprite.rgba.data(), assets_->sprite.width,
                                     assets_->sprite.height);
        if (image_ == kInvalidImage) {
            image_failed_ = true;
            log::warn("2D Puck: cannot upload the sprite; using the placeholder");
        } else {
            create_bands(canvas);
            assets_->sprite.rgba.clear();
            assets_->sprite.rgba.shrink_to_fit();
        }
    }

    if (image_ == kInvalidImage) {
        draw_placeholder(canvas, bounds, visual, time_seconds);
        return;
    }

    draw_assets(canvas, bounds, visual, time_seconds);
}

void Puck2D::create_bands(Canvas& canvas) {
    const Sprite& sprite = assets_->sprite;
    if (sprite.rgba.empty() || sprite.width <= 0 || sprite.height <= 0) {
        return;
    }
    const int band_height = std::max(1, (sprite.height + kBandCount - 1) / kBandCount);
    const std::size_t row_bytes = static_cast<std::size_t>(sprite.width) * 4u;
    std::vector<Band> bands;
    for (int y = 0; y < sprite.height; y += band_height) {
        const int height = std::min(band_height, sprite.height - y);
        const std::uint8_t* pixels =
            sprite.rgba.data() + static_cast<std::size_t>(y) * row_bytes;
        const ImageHandle image = canvas.create_image(pixels, sprite.width, height);
        if (image == kInvalidImage) {
            for (const Band& band : bands) {
                canvas.release_image(band.image);
            }
            return;
        }
        bands.push_back(Band{image, y, height});
    }
    bands_ = std::move(bands);
}

void Puck2D::release_image() {
    for (const Band& band : bands_) {
        if (band.image != kInvalidImage && canvas_) {
            canvas_->release_image(band.image);
        }
    }
    bands_.clear();
    if (image_ != kInvalidImage && canvas_) {
        canvas_->release_image(image_);
    }
    image_ = kInvalidImage;
    canvas_ = nullptr;
}

void Puck2D::draw_placeholder(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) {
    const float image_height = std::min(bounds.h, bounds.w / kPuckAspect);
    const float size = std::min(bounds.w, bounds.h) * 0.92f;
    const float cx = bounds.center_x();
    const float cy = bounds.center_y() + size * 0.015f;
    const float breathe = 1.0f + 0.012f * std::sin(time_seconds * 1.9f);

    const PuckPose& interaction = visual.pose;
    const float squash = std::max(0.4f, 1.0f + interaction.squash);
    const float squash_x = 1.0f / std::sqrt(squash);
    const float shift_x = interaction.shift_x * image_height;
    const float lift_y = interaction.lift_y * image_height;
    const float lean = std::clamp(interaction.roll + interaction.pitch +
                                      (interaction.grabbed ? 0.0f : body_turn_),
                                  -kMaxLean, kMaxLean);
    const float tan_lean = std::tan(lean);

    const float body_rx = size * 0.275f * squash_x;
    const float body_ry = size * 0.345f * breathe * squash;
    const float ground = cy + body_ry * 1.03f;
    const auto px = [&](float x, float y) { return x + shift_x + tan_lean * (y - ground); };
    const auto py = [&](float y) { return y - lift_y; };

    const Color skin = visual.not_listening ? hex(0x3f4233) : hex(0x505238);
    const Color skin_shadow = visual.not_listening ? hex(0x303226) : hex(0x3f412b);
    const Color sclera = visual.not_listening ? hex(0x9d9d8d) : hex(0xe3dec7);
    const Color pupil = hex(0x16160f);
    const Color line = visual.not_listening ? hex(0x24261c) : hex(0x2a2c1e);

    // Soft ground shadow.
    canvas.fill_ellipse(px(cx, ground), py(ground), body_rx * 0.78f, size * 0.017f,
                        hex(0x000000, 0.32f));

    // Legs and arms are intentionally simple shapes. This is the fallback when
    // the fetched avatar and face data are unavailable.
    canvas.fill_ellipse(px(cx - body_rx * 0.38f, cy + body_ry * 1.02f),
                        py(cy + body_ry * 1.02f), size * 0.044f, size * 0.155f, skin);
    canvas.fill_ellipse(px(cx + body_rx * 0.38f, cy + body_ry * 1.02f),
                        py(cy + body_ry * 1.02f), size * 0.044f, size * 0.155f, skin);
    canvas.fill_ellipse(px(cx - body_rx * 1.00f, cy + body_ry * 0.55f),
                        py(cy + body_ry * 0.55f), size * 0.032f, size * 0.180f, skin_shadow);
    canvas.fill_ellipse(px(cx + body_rx * 1.00f, cy + body_ry * 0.55f),
                        py(cy + body_ry * 0.55f), size * 0.032f, size * 0.180f, skin_shadow);

    // Body.
    canvas.fill_ellipse(px(cx, cy), py(cy), body_rx, body_ry, skin);
    canvas.fill_ellipse(px(cx - body_rx * 0.18f, cy - body_ry * 0.18f),
                        py(cy - body_ry * 0.18f), body_rx * 0.72f, body_ry * 0.70f,
                        hex(0x6a6d4a, 0.07f));

    // Eyes.
    const float eye_dx = body_rx * 0.42f;
    const float eye_y = cy - body_ry * 0.30f;
    const float eye_r = size * 0.052f;
    canvas.fill_circle(px(cx - eye_dx, eye_y), py(eye_y), eye_r, sclera);
    canvas.fill_circle(px(cx + eye_dx, eye_y), py(eye_y), eye_r, sclera);

    float gaze_x = 0.0f;
    float gaze_y = visual.thinking ? -0.65f : 0.0f;
    if (interaction.pointer_active && !visual.speaking && !visual.listening &&
        !visual.thinking && !visual.not_listening) {
        const float eye_center_x = px(cx, eye_y);
        const float eye_center_y = py(eye_y);
        const float dx = interaction.pointer_x - eye_center_x;
        const float dy = interaction.pointer_y - eye_center_y;
        const float distance = std::hypot(dx, dy);
        const float reach = std::max(160.0f, bounds.w * 1.5f);
        const float amount = std::min(1.0f, distance / reach);
        if (distance >= 1.0f) {
            gaze_x = dx / distance * amount;
            gaze_y = dy / distance * amount;
        }
    }
    const float pupil_dx = gaze_x * eye_r * 0.42f;
    const float pupil_dy = gaze_y * eye_r * 0.42f;
    canvas.fill_circle(px(cx - eye_dx, eye_y) + pupil_dx, py(eye_y) + pupil_dy, eye_r * 0.42f,
                       pupil);
    canvas.fill_circle(px(cx + eye_dx, eye_y) + pupil_dx, py(eye_y) + pupil_dy, eye_r * 0.42f,
                       pupil);

    if (visual.not_listening) {
        canvas.fill_ellipse(px(cx - eye_dx, eye_y - eye_r * 0.35f),
                            py(eye_y - eye_r * 0.35f), eye_r * 1.15f, eye_r * 0.55f, skin);
        canvas.fill_ellipse(px(cx + eye_dx, eye_y - eye_r * 0.35f),
                            py(eye_y - eye_r * 0.35f), eye_r * 1.15f, eye_r * 0.55f, skin);
    }

    // Nose.
    canvas.fill_triangle(px(cx, cy - body_ry * 0.10f), py(cy - body_ry * 0.10f),
                         px(cx - size * 0.018f, cy + body_ry * 0.16f),
                         py(cy + body_ry * 0.16f),
                         px(cx + size * 0.018f, cy + body_ry * 0.16f),
                         py(cy + body_ry * 0.16f), skin_shadow);

    // Mouth.
    const float mouth_y = cy + body_ry * 0.33f;
    if (visual.speaking) {
        const float open = 0.018f + 0.014f * std::abs(std::sin(time_seconds * 9.0f));
        canvas.fill_ellipse(px(cx, mouth_y), py(mouth_y), size * 0.038f, size * open, line);
    } else {
        canvas.stroke_line(px(cx - size * 0.085f, mouth_y), py(mouth_y),
                           px(cx + size * 0.085f, mouth_y), py(mouth_y), 2.2f, line);
    }
}

void Puck2D::draw_assets(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) {
    const Assets& assets = *assets_;
    const float time = std::max(0.0f, time_seconds);
    const float aspect = static_cast<float>(assets.sprite.width) / static_cast<float>(assets.sprite.height);
    const float image_height = std::min(bounds.h, bounds.w / aspect);
    const float image_width = image_height * aspect;
    const Rect image_rect{
        bounds.center_x() - image_width * 0.5f,
        bounds.center_y() - image_height * 0.5f,
        image_width,
        image_height,
    };

    const float phase = std::fmod(time, 3.2f) / 3.2f;
    const float breathe = visual.speaking ? 0.0f : 0.5f - 0.5f * std::cos(phase * 6.28318530718f);
    const PuckPose& interaction = visual.pose;
    const float squash = std::max(0.4f, 1.0f + interaction.squash);
    const float squash_x = 1.0f / std::sqrt(squash);
    const float breathe_x = (1.0f + 0.012f * breathe) * squash_x;
    const float breathe_y = (1.0f - 0.015f * breathe) * squash;
    const float lift = 0.004f * image_height * breathe - interaction.lift_y * image_height;
    const float lean = std::clamp(interaction.roll + interaction.pitch +
                                      (interaction.grabbed ? 0.0f : body_turn_),
                                  -kMaxLean, kMaxLean);

    Pose pose;
    pose.image = image_rect;
    pose.origin_x = image_rect.center_x() + interaction.shift_x * image_height;
    pose.origin_y = image_rect.y + image_rect.h * 0.95f;
    pose.scale_x = image_rect.w / static_cast<float>(assets.face.width > 0 ? assets.face.width : assets.sprite.width);
    pose.scale_y = image_rect.h / static_cast<float>(assets.face.height > 0 ? assets.face.height : assets.sprite.height);
    pose.breathe_x = breathe_x;
    pose.breathe_y = breathe_y;
    pose.lift = lift;
    pose.lean = lean;
    pose.pivot_y = pose.origin_y +
                   (image_rect.y + image_rect.h * kPuckGroundY - pose.origin_y) * breathe_y +
                   lift;

    const float dim = visual.not_listening ? 0.62f : 1.0f;
    const Rect sprite_rect{
        pose.origin_x + (image_rect.x - pose.origin_x) * breathe_x,
        pose.origin_y + (image_rect.y - pose.origin_y) * breathe_y + lift,
        image_rect.w * breathe_x,
        image_rect.h * breathe_y,
    };
    // Overlapping bands blend twice when the Puck is dimmed, so the muted
    // state keeps the single image and only shifts/scales.
    if (std::abs(lean) < 0.01f || bands_.empty() || visual.not_listening) {
        canvas.draw_image(image_, sprite_rect, dim);
    } else {
        // Overlap each band by a pixel: adjacent image patterns otherwise
        // leave a filtered seam line across the body.
        constexpr float kBandOverlap = 1.0f;
        for (const Band& band : bands_) {
            const float band_y = sprite_rect.y + static_cast<float>(band.y) * breathe_y;
            const float band_height = static_cast<float>(band.height) * breathe_y;
            const float dx = pose.shear(band_y + band_height * 0.5f);
            canvas.draw_image(band.image,
                              Rect{sprite_rect.x + dx, band_y - kBandOverlap * 0.5f,
                                   sprite_rect.w, band_height + kBandOverlap},
                              dim);
        }
    }

    if (!assets.face_ok) {
        return;
    }

    const bool first = last_time_ < 0.0f;
    const float dt = first ? 0.0f : std::clamp(time - last_time_, 0.0f, 0.1f);
    last_time_ = time;

    float target_x = 0.0f;
    float target_y = 0.0f;
    if (visual.thinking) {
        target_x = 0.5f * std::sin(time * 0.9f) + 0.08f;
        target_y = -0.65f + 0.08f * std::sin(time * 1.7f);
    } else if (visual.speaking || visual.listening) {
        target_y = 0.1f;
    } else {
        target_x = 0.45f * std::sin(time * 0.37f) + 0.15f * std::sin(time * 0.91f + 1.7f);
        target_y = 0.12f * std::sin(time * 0.53f + 0.6f);
    }
    if (interaction.pointer_active && !visual.speaking && !visual.listening &&
        !visual.thinking && !visual.not_listening) {
        // The web's eye tracker: aim at the pointer with a reach of
        // max(160 px, 1.5 x the Puck's width) and fall off to the edge.
        const float eye_x = image_rect.x + image_rect.w * kPuckEyeX;
        const float eye_y = image_rect.y + image_rect.h * kPuckEyeY;
        const float dx = interaction.pointer_x - eye_x;
        const float dy = interaction.pointer_y - eye_y;
        const float distance = std::hypot(dx, dy);
        const float reach = std::max(160.0f, bounds.w * 1.5f);
        const float amount = std::min(1.0f, distance / reach);
        if (distance < 1.0f) {
            target_x = 0.0f;
            target_y = 0.0f;
        } else {
            target_x = dx / distance * amount;
            target_y = dy / distance * amount;
        }
    }
    if (visual.not_listening) {
        target_x = 0.0f;
        target_y = 0.0f;
    }

    if (first) {
        gaze_x_ = target_x;
        gaze_y_ = target_y;
    } else {
        const float follow = 1.0f - std::exp(-dt * 18.0f);
        gaze_x_ += (target_x - gaze_x_) * follow;
        gaze_y_ += (target_y - gaze_y_) * follow;
    }

    // The web turns the body toward the smoothed gaze by 0.25 x/y at a 0.6 s
    // time constant, and drops the turn while a hold is active.
    const float body_target = interaction.grabbed ? 0.0f : gaze_y_ * 0.08f;
    body_turn_ += (body_target - body_turn_) *
                  (first ? 1.0f : 1.0f - std::exp(-dt / 0.6f));

    const float blink_phase = std::fmod(time + 1.3f, 4.6f);
    float blink = 0.0f;
    if (blink_phase < 0.07f) {
        blink = blink_phase / 0.07f;
    } else if (blink_phase < 0.18f) {
        blink = 1.0f - (blink_phase - 0.07f) / 0.11f;
    }
    const float lid_target = visual.not_listening ? 1.0f : 0.0f;
    if (first) {
        lids_ = lid_target;
    } else {
        lids_ += (lid_target - lids_) * (1.0f - std::exp(-dt * 14.0f));
    }
    blink = std::max(blink, lids_);

    float mouth_target = std::clamp(visual.mouth, 0.0f, 1.0f);
    if (visual.speaking) {
        const float speech = 0.16f + 0.34f * (0.5f + 0.5f * std::sin(time * 8.7f)) *
                                          (0.65f + 0.35f * std::sin(time * 3.1f + 1.2f));
        mouth_target = std::max(mouth_target, speech);
    }
    if (first) {
        mouth_open_ = mouth_target;
    } else {
        const float rate = mouth_target > mouth_open_ ? 18.0f : 24.0f;
        mouth_open_ += (mouth_target - mouth_open_) * (1.0f - std::exp(-dt * rate));
    }
    mouth_open_ = std::clamp(mouth_open_, 0.0f, 1.0f);

    const auto draw_eye = [&](const Eye& eye) {
        const float x = pose.x(eye.x, eye.y);
        const float y = pose.y(eye.y);
        const float ax = pose.rx(eye.r) * 1.07f;
        const float ay = pose.ry(eye.r) * 1.07f;

        // Sclera gradient: a thin dark rim, the base color, then a highlight
        // toward the upper left, matching the web's radial gradient.
        canvas.fill_ellipse(x, y, ax, ay, fade(shade(assets.face.sclera, -0.20f), dim));
        canvas.fill_ellipse(x, y, ax * 0.97f, ay * 0.97f, fade(assets.face.sclera, dim));
        canvas.fill_ellipse(x - ax * 0.10f, y - ay * 0.12f, ax * 0.82f, ay * 0.82f,
                            fade(shade(assets.face.sclera, 0.04f), dim));
        canvas.fill_ellipse(x - ax * 0.18f, y - ay * 0.22f, ax * 0.62f, ay * 0.62f,
                            fade(shade(assets.face.sclera, 0.10f), dim));
        canvas.fill_ellipse(x - ax * 0.24f, y - ay * 0.30f, ax * 0.40f, ay * 0.40f,
                            fade(shade(assets.face.sclera, 0.14f), dim));

        const float gaze_x = std::clamp(gaze_x_, -1.0f, 1.0f);
        const float gaze_y = std::clamp(gaze_y_, -1.0f, 1.0f);
        const float pupil_x = x + gaze_x * pose.rx(eye.r) * 0.42f;
        const float pupil_y = y + gaze_y * pose.ry(eye.r) * 0.42f;
        const float pupil_rx = pose.rx(eye.r) * 0.44f;
        const float pupil_ry = pose.ry(eye.r) * 0.44f;
        canvas.fill_ellipse(pupil_x, pupil_y, pupil_rx, pupil_ry, fade(hex(0x0d0b0a), dim));
        canvas.fill_ellipse(pupil_x, pupil_y, pupil_rx * 0.88f, pupil_ry * 0.88f,
                            fade(hex(0x161310), dim));
        canvas.fill_ellipse(pupil_x, pupil_y, pupil_rx * 0.65f, pupil_ry * 0.65f,
                            fade(hex(0x211d19), dim));
        canvas.fill_ellipse(pupil_x, pupil_y, pupil_rx * 0.42f, pupil_ry * 0.42f,
                            fade(hex(0x2a2622), dim));
        canvas.fill_ellipse(pupil_x - pupil_rx * 0.32f, pupil_y - pupil_ry * 0.36f,
                            pupil_rx * 0.26f, pupil_ry * 0.26f,
                            fade(hex(0xffffff), 0.85f * dim));

        // The upper lid shadow sits over the top of the pupil, as in the web.
        canvas.fill_ellipse(x, y - ay * 0.80f, ax * 0.70f, ay * 0.18f,
                            fade(hex(0x000000), 0.12f * dim));

        if (blink > 0.01f) {
            const float lid_center = y - ay + blink * ay * 2.1f - ay;
            constexpr int kLidSlices = 48;
            const float slice_width = (2.0f * ax) / static_cast<float>(kLidSlices);
            // The lid has to be opaque: a translucent lid would let the sprite's
            // painted eye show through when the eyes are shut.
            const Color lid_base = shade(assets.face.skin, dim < 1.0f ? -0.32f : -0.10f);
            for (int slice = 0; slice < kLidSlices; ++slice) {
                const float x0 = x - ax + slice_width * static_cast<float>(slice);
                const float x1 = x0 + slice_width + 0.6f;
                const float mid = (x0 + x1) * 0.5f;
                const float nx = (mid - x) / ax;
                if (std::abs(nx) >= 1.0f) {
                    continue;
                }
                const float circle = std::sqrt(std::max(0.0f, 1.0f - nx * nx));
                const float eye_top = y - ay * circle;
                const float lid_bottom = lid_center + ay * circle;
                const float cover = std::min(lid_bottom, y + ay * circle);
                if (cover <= eye_top) {
                    continue;
                }
                const float width = x1 - x0;
                const float height = cover - eye_top;
                canvas.fill_rect(Rect{x0, eye_top, width, height}, lid_base);
                const float band1 = eye_top + height * 0.35f;
                canvas.fill_rect(Rect{x0, band1, width, cover - band1}, shade(lid_base, 0.06f));
                const float band2 = eye_top + height * 0.65f;
                canvas.fill_rect(Rect{x0, band2, width, cover - band2}, shade(lid_base, 0.12f));
                const float band3 = eye_top + height * 0.85f;
                canvas.fill_rect(Rect{x0, band3, width, cover - band3}, shade(lid_base, 0.18f));
            }

            if (blink < 0.85f) {
                constexpr int kLashSegments = 16;
                for (int segment = 0; segment < kLashSegments; ++segment) {
                    const float n0 = -1.0f + 2.0f * static_cast<float>(segment) / kLashSegments;
                    const float n1 = -1.0f + 2.0f * static_cast<float>(segment + 1) / kLashSegments;
                    const float y0 = lid_center + ay * std::sqrt(std::max(0.0f, 1.0f - n0 * n0));
                    const float y1 = lid_center + ay * std::sqrt(std::max(0.0f, 1.0f - n1 * n1));
                    canvas.stroke_line(x + ax * n0, y0, x + ax * n1, y1,
                                       std::max(1.0f, ax * 0.08f),
                                       fade(hex(0x000000), 0.45f * dim));
                }
            }

            if (blink > 0.6f) {
                const float lash = (blink - 0.6f) / 0.4f;
                constexpr int kLashSegments = 16;
                for (int segment = 0; segment < kLashSegments; ++segment) {
                    const float n0 = -0.7f + 1.4f * static_cast<float>(segment) / kLashSegments;
                    const float n1 = -0.7f + 1.4f * static_cast<float>(segment + 1) / kLashSegments;
                    const float y0 = y + ay * (0.05f + 0.25f * (1.0f - (n0 / 0.7f) * (n0 / 0.7f)));
                    const float y1 = y + ay * (0.05f + 0.25f * (1.0f - (n1 / 0.7f) * (n1 / 0.7f)));
                    canvas.stroke_line(x + ax * n0, y0, x + ax * n1, y1,
                                       std::max(0.8f, ax * 0.14f),
                                       fade(hex(0x000000), 0.6f * lash * dim));
                }
            }
        }

        canvas.stroke_circle(x, y, (ax + ay) * 0.5f, std::max(0.75f, (ax + ay) * 0.5f * 0.05f),
                             fade(hex(0x000000), 0.30f * dim));
    };

    draw_eye(assets.face.left_eye);
    draw_eye(assets.face.right_eye);

    if (mouth_open_ > 0.01f) {
        const float scale = assets.face.mouth.half_width / kMouthCanonicalHalfWidth;
        const float upper = kMouthOpenUpper * mouth_open_;
        const float lower = kMouthOpenLower * mouth_open_;
        const float half_width = assets.face.mouth.half_width;
        const float mouth_half_width = half_width * (1.0f - 0.1f * mouth_open_);
        const float alpha = std::min(1.0f, mouth_open_ / 0.08f);
        const float halo_center = assets.face.mouth.y + (lower - upper) * 0.5f * scale;
        const float halo_radius = (2.0f * kMouthCanonicalLine + upper + lower) * 0.5f * scale;
        canvas.fill_ellipse(pose.x(assets.face.mouth.x, halo_center), pose.y(halo_center),
                            pose.rx(mouth_half_width * 1.04f), pose.ry(halo_radius * 1.12f),
                            fade(hex(0x0d0f06), 0.30f * alpha * dim));

        // The web's mouth is a lens between two curves that fall off toward the
        // corners; vertical slices reproduce that shape with flat primitives.
        constexpr int kMouthSlices = 64;
        for (int slice = 0; slice < kMouthSlices; ++slice) {
            const float n0 = -1.0f + 2.0f * static_cast<float>(slice) / kMouthSlices;
            const float n1 = -1.0f + 2.0f * static_cast<float>(slice + 1) / kMouthSlices;
            const float mid = (n0 + n1) * 0.5f;
            const float falloff = std::max(0.0f, 1.0f - mid * mid);
            const float upper_curve = std::pow(falloff, 0.72f);
            const float lower_curve = std::pow(falloff, 0.62f);
            const float baseline = assets.face.mouth.y +
                                   assets.face.mouth.sag * half_width * mid * mid;
            const float y_upper = baseline - (kMouthCanonicalLine + upper) * upper_curve * scale;
            const float y_lower = baseline + (kMouthCanonicalLine + lower) * lower_curve * scale;
            if (y_lower <= y_upper) {
                continue;
            }

            const float mid_y = (y_upper + y_lower) * 0.5f;
            const float x0 = pose.x(assets.face.mouth.x + mouth_half_width * n0, mid_y);
            const float x1 =
                pose.x(assets.face.mouth.x + mouth_half_width * n1 + 0.5f, mid_y);
            const float top = pose.y(y_upper);
            const float bottom = pose.y(y_lower);
            const float height = bottom - top;
            canvas.fill_rect(Rect{x0, top, x1 - x0, height},
                             fade(hex(0x1a1e0d), alpha * dim));

            const float gradient = top + height * 0.62f;
            canvas.fill_rect(Rect{x0, gradient, x1 - x0, bottom - gradient},
                             fade(hex(0x2f3418), 0.85f * alpha * dim));

            const float lip_height = std::max(1.0f, height * 0.08f);
            const float lip_top = std::max(top, bottom - lip_height);
            canvas.fill_rect(Rect{x0, lip_top, x1 - x0, bottom - lip_top},
                             fade(hex(0xd8dcae), 0.20f * alpha * dim));
        }
    }
}

}  // namespace pk
