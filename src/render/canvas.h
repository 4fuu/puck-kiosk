#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>

namespace pk {

enum class TextAlign {
    Left,
    Center,
    Right,
};

// Where y sits relative to the glyph box. UI layout normally centers text in a
// line box (Middle); Baseline is for callers that already know the baseline.
enum class TextVAlign {
    Top,
    Middle,
    Baseline,
};

// The car page uses 400, 500, and 600. A backend with a single font face may
// synthesize the heavier weights.
enum class FontWeight {
    Regular,
    Medium,
    Semibold,
};

struct TextStyle {
    float size = 16.0f;
    Color color;
    TextAlign align = TextAlign::Left;
    TextVAlign valign = TextVAlign::Middle;
    FontWeight weight = FontWeight::Regular;
    // Extra spacing per character, in em, matching CSS letter-spacing.
    float tracking = 0.0f;
};

// A backend-owned raster texture. Zero means "no image".
using ImageHandle = std::uint64_t;
constexpr ImageHandle kInvalidImage = 0;

// The 2D drawing seam. NanoVG is the M0 backend; a Skia, LVGL, or software
// backend can implement the same calls on a device where NanoVG is a poor fit.
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual bool initialize(const std::string& font_path, std::string* error) = 0;
    virtual void begin_frame(Size size, float dpi_scale) = 0;
    virtual void end_frame() = 0;

    // Makes every drawing call issued so far visible without ending the frame.
    // A renderer that draws with raw GL between 2D calls can flush here, so the
    // 3D pass lands over the background and later 2D drawing lands over it.
    // Drawing state does not survive a flush: backends reset to default, and
    // callers set their state on every call. The default is a no-op for
    // backends that draw immediately.
    virtual void flush() {}

    // Takes straight-alpha RGBA8 pixels. The caller owns the handle and must
    // release it before the canvas is destroyed.
    virtual ImageHandle create_image(const std::uint8_t* rgba, int width, int height) = 0;
    virtual void draw_image(ImageHandle image, Rect rect, float alpha) = 0;
    virtual void release_image(ImageHandle image) = 0;

    virtual void clear(Color color) = 0;
    virtual void fill_rect(Rect rect, Color color) = 0;
    virtual void fill_rounded_rect(Rect rect, float radius, Color color) = 0;
    virtual void fill_circle(float center_x, float center_y, float radius, Color color) = 0;
    virtual void fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color) = 0;
    virtual void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color color) = 0;
    virtual void stroke_circle(float center_x, float center_y, float radius, float width, Color color) = 0;
    virtual void stroke_line(float x0, float y0, float x1, float y1, float width, Color color) = 0;

    virtual void text(float x, float y, const TextStyle& style, const std::string& value) = 0;

    // Width of the laid-out text, including tracking and synthetic weight.
    virtual float measure_text(const TextStyle& style, const std::string& value) = 0;

    // Fills an SVG path (M/L/H/V/C/S/Q/T/Z, absolute and relative) scaled from
    // view_box into dest, preserving aspect and centering. Winding is ignored.
    virtual void fill_path(const std::string& d, Rect view_box, Rect dest, Color color) = 0;

    float measure_text(float size, const std::string& value) {
        TextStyle style;
        style.size = size;
        return measure_text(style, value);
    }
};

}  // namespace pk
