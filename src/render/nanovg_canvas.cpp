#include "render/nanovg_canvas.h"

#include "core/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <nanovg.h>
#if defined(PUCK_KIOSK_CANVAS_NANOVG_GL3)
#include <nanovg_gl3_api.h>
#else
#include <nanovg_gles3_api.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace pk {
namespace {

NVGcolor to_nvg(Color color) {
    return nvgRGBAf(color.r, color.g, color.b, color.a);
}

std::string default_font_path() {
    std::vector<std::string> candidates;
    const char* windir = std::getenv("WINDIR");
    const std::string windows = windir && *windir ? windir : "C:/Windows";
    candidates.push_back(windows + "/Fonts/segoeui.ttf");
    candidates.push_back(windows + "/Fonts/arial.ttf");
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    candidates.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    for (const std::string& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string bold_sibling(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path font(path);
    const std::string extension = font.extension().string();
    const std::string stem = font.stem().string();
    std::vector<std::string> names;
    const auto replace = [&](const std::string& from, const std::string& to) {
        std::string value = stem;
        const std::size_t position = value.find(from);
        if (position != std::string::npos) {
            value.replace(position, from.size(), to);
            names.push_back(value + extension);
        }
    };
    replace("Regular", "Bold");
    replace("regular", "bold");
    names.push_back(stem + "-Bold" + extension);
    names.push_back(stem + "Bold" + extension);
    names.push_back(stem + "-Semibold" + extension);
    // Windows system fonts use compact suffixes: segoeuib.ttf, arialbd.ttf.
    names.push_back(stem + "b" + extension);
    names.push_back(stem + "bd" + extension);
    for (const std::string& name : names) {
        const fs::path candidate = font.parent_path() / name;
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }
    return {};
}

int to_align(TextAlign align, TextVAlign valign) {
    int flags = NVG_ALIGN_LEFT;
    switch (align) {
        case TextAlign::Left: flags = NVG_ALIGN_LEFT; break;
        case TextAlign::Center: flags = NVG_ALIGN_CENTER; break;
        case TextAlign::Right: flags = NVG_ALIGN_RIGHT; break;
    }
    switch (valign) {
        case TextVAlign::Top: return flags | NVG_ALIGN_TOP;
        case TextVAlign::Middle: return flags | NVG_ALIGN_MIDDLE;
        case TextVAlign::Baseline: return flags | NVG_ALIGN_BASELINE;
    }
    return flags | NVG_ALIGN_MIDDLE;
}

// NanoVG has one face per font name. Prefer a Bold sibling when the operator
// font ships one; otherwise synthesize the car page's 500/600 weights by
// drawing the glyphs twice with a sub-pixel offset.
float weight_offset(const TextStyle& style, bool bold_face) {
    switch (style.weight) {
        case FontWeight::Regular: return 0.0f;
        case FontWeight::Medium: return style.size * 0.020f;
        case FontWeight::Semibold: return bold_face ? 0.0f : style.size * 0.035f;
    }
    return 0.0f;
}

void apply_text_state(NVGcontext* context, const TextStyle& style, bool bold_face) {
    nvgFontFace(context,
                style.weight == FontWeight::Semibold && bold_face ? "ui-bold" : "ui");
    nvgFontSize(context, style.size);
    nvgTextAlign(context, to_align(style.align, style.valign));
    nvgTextLetterSpacing(context, style.tracking * style.size);
}

}  // namespace

NanoVgCanvas::~NanoVgCanvas() {
    if (context_) {
#if defined(PUCK_KIOSK_CANVAS_NANOVG_GL3)
        nvgDeleteGL3(context_);
#else
        nvgDeleteGLES3(context_);
#endif
        context_ = nullptr;
    }
}

bool NanoVgCanvas::initialize(const std::string& font_path, std::string* error) {
#if defined(PUCK_KIOSK_CANVAS_NANOVG_GL3)
    context_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!context_) {
        if (error) {
            *error = "nvgCreateGL3 failed";
        }
        return false;
    }
#else
    context_ = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!context_) {
        if (error) {
            *error = "nvgCreateGLES3 failed";
        }
        return false;
    }
#endif

    const std::string path = font_path.empty() ? default_font_path() : font_path;
    if (path.empty()) {
        if (error) {
            *error = "no UI font found; pass --font /path/to/font.ttf";
        }
        return false;
    }
    if (nvgCreateFont(context_, "ui", path.c_str()) == -1) {
        if (error) {
            *error = "cannot load UI font: " + path;
        }
        return false;
    }
    log::info("UI font: %s", path.c_str());
    const std::string bold_path = bold_sibling(path);
    if (!bold_path.empty() && nvgCreateFont(context_, "ui-bold", bold_path.c_str()) != -1) {
        bold_face_ = true;
        log::info("UI bold font: %s", bold_path.c_str());
    }
    return true;
}

void NanoVgCanvas::begin_frame(Size size, float dpi_scale) {
    size_ = size;
    dpi_scale_ = dpi_scale;
    translate_x_ = 0.0f;
    translate_y_ = 0.0f;
    transform_stack_.clear();
    nvgBeginFrame(context_, static_cast<float>(size.width), static_cast<float>(size.height), dpi_scale);
    frame_open_ = true;
}

void NanoVgCanvas::end_frame() {
    if (frame_open_) {
        nvgEndFrame(context_);
        frame_open_ = false;
    }
}

void NanoVgCanvas::flush() {
    if (!context_ || !frame_open_) {
        return;
    }
    // End the frame to submit the queued 2D geometry, then start a fresh one.
    // Drawing state resets to default; every Canvas call sets its own state.
    // The transform is ours, so put it back for the drawing after the flush.
    nvgEndFrame(context_);
    nvgBeginFrame(context_, static_cast<float>(size_.width), static_cast<float>(size_.height), dpi_scale_);
    apply_translation();
}

void NanoVgCanvas::apply_translation() {
    if (!context_) {
        return;
    }
    nvgResetTransform(context_);
    nvgTranslate(context_, translate_x_, translate_y_);
}

void NanoVgCanvas::save() {
    if (!context_) {
        return;
    }
    nvgSave(context_);
    transform_stack_.push_back(SavedTransform{translate_x_, translate_y_});
}

void NanoVgCanvas::restore() {
    if (!context_) {
        return;
    }
    nvgRestore(context_);
    if (!transform_stack_.empty()) {
        translate_x_ = transform_stack_.back().x;
        translate_y_ = transform_stack_.back().y;
        transform_stack_.pop_back();
    } else {
        translate_x_ = 0.0f;
        translate_y_ = 0.0f;
    }
    // A flush resets NanoVG's state stack; reapply our translation so restore
    // lands in the same place whether or not the caller flushed.
    apply_translation();
}

void NanoVgCanvas::translate(float dx, float dy) {
    if (!context_) {
        return;
    }
    nvgTranslate(context_, dx, dy);
    translate_x_ += dx;
    translate_y_ += dy;
}

void NanoVgCanvas::translation(float* x, float* y) const {
    if (x) {
        *x = translate_x_;
    }
    if (y) {
        *y = translate_y_;
    }
}

ImageHandle NanoVgCanvas::create_image(const std::uint8_t* rgba, int width, int height) {
    if (!context_ || !rgba || width <= 0 || height <= 0) {
        return kInvalidImage;
    }
    const int image = nvgCreateImageRGBA(context_, width, height, 0, rgba);
    return image > 0 ? static_cast<ImageHandle>(image) : kInvalidImage;
}

void NanoVgCanvas::draw_image(ImageHandle image, Rect rect, float alpha) {
    if (!context_ || image == kInvalidImage || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const NVGpaint paint = nvgImagePattern(context_, rect.x, rect.y, rect.w, rect.h, 0.0f,
                                           static_cast<int>(image), alpha);
    nvgBeginPath(context_);
    nvgRect(context_, rect.x, rect.y, rect.w, rect.h);
    nvgFillPaint(context_, paint);
    nvgFill(context_);
}

void NanoVgCanvas::release_image(ImageHandle image) {
    if (context_ && image != kInvalidImage) {
        nvgDeleteImage(context_, static_cast<int>(image));
    }
}

void NanoVgCanvas::clear(Color color) {
    fill_rect(Rect{0.0f, 0.0f, static_cast<float>(size_.width), static_cast<float>(size_.height)}, color);
}

void NanoVgCanvas::fill_rect(Rect rect, Color color) {
    nvgBeginPath(context_);
    nvgRect(context_, rect.x, rect.y, rect.w, rect.h);
    nvgFillColor(context_, to_nvg(color));
    nvgFill(context_);
}

void NanoVgCanvas::fill_rounded_rect(Rect rect, float radius, Color color) {
    nvgBeginPath(context_);
    nvgRoundedRect(context_, rect.x, rect.y, rect.w, rect.h, radius);
    nvgFillColor(context_, to_nvg(color));
    nvgFill(context_);
}

void NanoVgCanvas::fill_circle(float center_x, float center_y, float radius, Color color) {
    nvgBeginPath(context_);
    nvgCircle(context_, center_x, center_y, radius);
    nvgFillColor(context_, to_nvg(color));
    nvgFill(context_);
}

void NanoVgCanvas::fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color) {
    nvgBeginPath(context_);
    nvgEllipse(context_, center_x, center_y, radius_x, radius_y);
    nvgFillColor(context_, to_nvg(color));
    nvgFill(context_);
}

void NanoVgCanvas::fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color color) {
    nvgBeginPath(context_);
    nvgMoveTo(context_, x0, y0);
    nvgLineTo(context_, x1, y1);
    nvgLineTo(context_, x2, y2);
    nvgClosePath(context_);
    nvgFillColor(context_, to_nvg(color));
    nvgFill(context_);
}

void NanoVgCanvas::stroke_circle(float center_x, float center_y, float radius, float width, Color color) {
    nvgBeginPath(context_);
    nvgCircle(context_, center_x, center_y, radius);
    nvgStrokeWidth(context_, width);
    nvgStrokeColor(context_, to_nvg(color));
    nvgStroke(context_);
}

void NanoVgCanvas::stroke_line(float x0, float y0, float x1, float y1, float width, Color color) {
    nvgBeginPath(context_);
    nvgMoveTo(context_, x0, y0);
    nvgLineTo(context_, x1, y1);
    nvgStrokeWidth(context_, width);
    nvgStrokeColor(context_, to_nvg(color));
    nvgStroke(context_);
}

void NanoVgCanvas::text(float x, float y, const TextStyle& style, const std::string& value) {
    apply_text_state(context_, style, bold_face_);
    nvgFillColor(context_, to_nvg(style.color));
    float draw_y = y;
    if (style.valign == TextVAlign::Middle) {
        // The font's metric middle sits above the ink middle for most faces;
        // center the actual glyph box so labels look centered in their rects.
        nvgTextAlign(context_, to_align(style.align, TextVAlign::Baseline));
        float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        nvgTextInkBounds(context_, 0.0f, 0.0f, value.c_str(), nullptr, bounds);
        draw_y = y - (bounds[1] + bounds[3]) * 0.5f;
    }
    const float offset = weight_offset(style, bold_face_);
    if (offset <= 0.0f) {
        nvgText(context_, x, draw_y, value.c_str(), nullptr);
        return;
    }
    float anchor = x;
    if (style.align == TextAlign::Center) {
        anchor -= offset * 0.5f;
    } else if (style.align == TextAlign::Right) {
        anchor -= offset;
    }
    nvgText(context_, anchor, draw_y, value.c_str(), nullptr);
    nvgText(context_, anchor + offset, draw_y, value.c_str(), nullptr);
}

float NanoVgCanvas::measure_text(const TextStyle& style, const std::string& value) {
    apply_text_state(context_, style, bold_face_);
    float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    nvgTextBounds(context_, 0.0f, 0.0f, value.c_str(), nullptr, bounds);
    return bounds[2] - bounds[0] + weight_offset(style, bold_face_);
}

void NanoVgCanvas::fill_path(const std::string& d, Rect view_box, Rect dest, Color color) {
    if (!context_ || d.empty() || view_box.w <= 0.0f || view_box.h <= 0.0f ||
        dest.w <= 0.0f || dest.h <= 0.0f) {
        return;
    }
    const float scale = std::min(dest.w / view_box.w, dest.h / view_box.h);
    const float ox = dest.x + (dest.w - view_box.w * scale) * 0.5f - view_box.x * scale;
    const float oy = dest.y + (dest.h - view_box.h * scale) * 0.5f - view_box.y * scale;
    const auto px = [&](float x) { return ox + x * scale; };
    const auto py = [&](float y) { return oy + y * scale; };

    std::size_t i = 0;
    const auto skip_sep = [&]() {
        while (i < d.size() && (d[i] == ' ' || d[i] == ',' || d[i] == '\n' || d[i] == '\t')) {
            ++i;
        }
    };
    const auto read_num = [&](float* out) {
        skip_sep();
        const std::size_t start = i;
        if (i < d.size() && (d[i] == '-' || d[i] == '+')) {
            ++i;
        }
        while (i < d.size() && (std::isdigit(static_cast<unsigned char>(d[i])) || d[i] == '.')) {
            ++i;
        }
        if (i < d.size() && (d[i] == 'e' || d[i] == 'E')) {
            ++i;
            if (i < d.size() && (d[i] == '-' || d[i] == '+')) {
                ++i;
            }
            while (i < d.size() && std::isdigit(static_cast<unsigned char>(d[i]))) {
                ++i;
            }
        }
        if (i == start) {
            return false;
        }
        *out = std::strtof(d.substr(start, i - start).c_str(), nullptr);
        return true;
    };

    float cx = 0, cy = 0, sx = 0, sy = 0, qx = 0, qy = 0, kx = 0, ky = 0;
    char cmd = 0;
    nvgBeginPath(context_);
    while (i < d.size()) {
        skip_sep();
        if (i >= d.size()) {
            break;
        }
        if (std::isalpha(static_cast<unsigned char>(d[i]))) {
            cmd = d[i++];
        }
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        const float bx = rel ? cx : 0.0f;
        const float by = rel ? cy : 0.0f;
        float a, b, c, e, f, g;
        switch (up) {
            case 'M':
                if (!read_num(&a) || !read_num(&b)) { i = d.size(); break; }
                cx = bx + a; cy = by + b; sx = cx; sy = cy;
                nvgMoveTo(context_, px(cx), py(cy));
                cmd = rel ? 'l' : 'L';
                break;
            case 'L':
                if (!read_num(&a) || !read_num(&b)) { i = d.size(); break; }
                cx = bx + a; cy = by + b;
                nvgLineTo(context_, px(cx), py(cy));
                break;
            case 'H':
                if (!read_num(&a)) { i = d.size(); break; }
                cx = bx + a;
                nvgLineTo(context_, px(cx), py(cy));
                break;
            case 'V':
                if (!read_num(&a)) { i = d.size(); break; }
                cy = by + a;
                nvgLineTo(context_, px(cx), py(cy));
                break;
            case 'C':
                if (!read_num(&a) || !read_num(&b) || !read_num(&c) || !read_num(&e) ||
                    !read_num(&f) || !read_num(&g)) { i = d.size(); break; }
                kx = bx + c; ky = by + e;
                cx = bx + f; cy = by + g;
                nvgBezierTo(context_, px(bx + a), py(by + b), px(kx), py(ky), px(cx), py(cy));
                break;
            case 'S':
                if (!read_num(&a) || !read_num(&b) || !read_num(&c) || !read_num(&e)) { i = d.size(); break; }
                {
                    const float rx = 2 * cx - kx;
                    const float ry = 2 * cy - ky;
                    kx = bx + a; ky = by + b;
                    cx = bx + c; cy = by + e;
                    nvgBezierTo(context_, px(rx), py(ry), px(kx), py(ky), px(cx), py(cy));
                }
                break;
            case 'Q':
                if (!read_num(&a) || !read_num(&b) || !read_num(&c) || !read_num(&e)) { i = d.size(); break; }
                qx = bx + a; qy = by + b;
                cx = bx + c; cy = by + e;
                nvgQuadTo(context_, px(qx), py(qy), px(cx), py(cy));
                break;
            case 'T':
                if (!read_num(&a) || !read_num(&b)) { i = d.size(); break; }
                qx = 2 * cx - qx; qy = 2 * cy - qy;
                cx = bx + a; cy = by + b;
                nvgQuadTo(context_, px(qx), py(qy), px(cx), py(cy));
                break;
            case 'Z':
                nvgClosePath(context_);
                cx = sx; cy = sy;
                break;
            default:
                i = d.size();
                break;
        }
    }
    nvgPathWinding(context_, NVG_SOLID);
    nvgFillColor(context_, to_nvg(color));
    nvgFill(context_);
}

}  // namespace pk
