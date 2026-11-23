#pragma once

#include "core/types.h"

#include <string>

namespace pk {

enum class TextAlign {
    Left,
    Center,
    Right,
};

// The 2D drawing seam. NanoVG is the M0 backend; a Skia, LVGL, or software
// backend can implement the same calls on a device where NanoVG is a poor fit.
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual bool initialize(const std::string& font_path, std::string* error) = 0;
    virtual void begin_frame(Size size, float dpi_scale) = 0;
    virtual void end_frame() = 0;

    virtual void clear(Color color) = 0;
    virtual void fill_rect(Rect rect, Color color) = 0;
    virtual void fill_rounded_rect(Rect rect, float radius, Color color) = 0;
    virtual void fill_circle(float center_x, float center_y, float radius, Color color) = 0;
    virtual void fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color) = 0;
    virtual void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color color) = 0;
    virtual void stroke_circle(float center_x, float center_y, float radius, float width, Color color) = 0;
    virtual void stroke_line(float x0, float y0, float x1, float y1, float width, Color color) = 0;

    virtual void text(float x, float y, float size, Color color, TextAlign align,
                      const std::string& value) = 0;
    virtual float measure_text(float size, const std::string& value) = 0;
};

}  // namespace pk
