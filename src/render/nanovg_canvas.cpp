#include "render/nanovg_canvas.h"

#include "core/log.h"

#include <nanovg.h>
#include <nanovg_gles3_api.h>

#include <filesystem>

namespace pk {
namespace {

NVGcolor to_nvg(Color color) {
    return nvgRGBAf(color.r, color.g, color.b, color.a);
}

std::string default_font_path() {
    const char* candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    };
    for (const char* candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

int to_align(TextAlign align) {
    switch (align) {
        case TextAlign::Left: return NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
        case TextAlign::Center: return NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE;
        case TextAlign::Right: return NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE;
    }
    return NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
}

}  // namespace

NanoVgCanvas::~NanoVgCanvas() {
    if (context_) {
        nvgDeleteGLES3(context_);
        context_ = nullptr;
    }
}

bool NanoVgCanvas::initialize(const std::string& font_path, std::string* error) {
    context_ = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!context_) {
        if (error) {
            *error = "nvgCreateGLES3 failed";
        }
        return false;
    }

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
    return true;
}

void NanoVgCanvas::begin_frame(Size size, float dpi_scale) {
    size_ = size;
    nvgBeginFrame(context_, static_cast<float>(size.width), static_cast<float>(size.height), dpi_scale);
    frame_open_ = true;
}

void NanoVgCanvas::end_frame() {
    if (frame_open_) {
        nvgEndFrame(context_);
        frame_open_ = false;
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

void NanoVgCanvas::text(float x, float y, float size, Color color, TextAlign align,
                        const std::string& value) {
    nvgFontFace(context_, "ui");
    nvgFontSize(context_, size);
    nvgTextAlign(context_, to_align(align));
    nvgFillColor(context_, to_nvg(color));
    nvgText(context_, x, y, value.c_str(), nullptr);
}

float NanoVgCanvas::measure_text(float size, const std::string& value) {
    nvgFontFace(context_, "ui");
    nvgFontSize(context_, size);
    float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    nvgTextBounds(context_, 0.0f, 0.0f, value.c_str(), nullptr, bounds);
    return bounds[2] - bounds[0];
}

}  // namespace pk
