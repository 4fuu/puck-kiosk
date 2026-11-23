#pragma once

#include "render/canvas.h"

struct NVGcontext;

namespace pk {

class NanoVgCanvas final : public Canvas {
public:
    ~NanoVgCanvas() override;

    bool initialize(const std::string& font_path, std::string* error) override;
    void begin_frame(Size size, float dpi_scale) override;
    void end_frame() override;
    void flush() override;

    ImageHandle create_image(const std::uint8_t* rgba, int width, int height) override;
    void draw_image(ImageHandle image, Rect rect, float alpha) override;
    void release_image(ImageHandle image) override;

    void clear(Color color) override;
    void fill_rect(Rect rect, Color color) override;
    void fill_rounded_rect(Rect rect, float radius, Color color) override;
    void fill_circle(float center_x, float center_y, float radius, Color color) override;
    void fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color) override;
    void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color color) override;
    void stroke_circle(float center_x, float center_y, float radius, float width, Color color) override;
    void stroke_line(float x0, float y0, float x1, float y1, float width, Color color) override;

    void text(float x, float y, const TextStyle& style, const std::string& value) override;
    float measure_text(const TextStyle& style, const std::string& value) override;

private:
    NVGcontext* context_ = nullptr;
    Size size_;
    float dpi_scale_ = 1.0f;
    bool frame_open_ = false;
    bool bold_face_ = false;
};

}  // namespace pk
