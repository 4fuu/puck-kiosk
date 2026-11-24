#pragma once

#include "render/canvas.h"

#include <vector>

struct NVGcontext;

namespace pk {

class NanoVgCanvas final : public Canvas {
public:
    ~NanoVgCanvas() override;

    bool initialize(const std::string& font_path, std::string* error) override;
    void begin_frame(Size size, float dpi_scale) override;
    void end_frame() override;
    void flush() override;
    void save() override;
    void restore() override;
    void translate(float dx, float dy) override;
    void translation(float* x, float* y) const override;
    float pixel_scale() const override { return dpi_scale_; }

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
    void fill_path(const std::string& d, Rect view_box, Rect dest, Color color) override;

private:
    struct SavedTransform {
        float x = 0.0f;
        float y = 0.0f;
    };

    void apply_translation();

    NVGcontext* context_ = nullptr;
    Size size_;
    float dpi_scale_ = 1.0f;
    bool frame_open_ = false;
    bool bold_face_ = false;
    float translate_x_ = 0.0f;
    float translate_y_ = 0.0f;
    std::vector<SavedTransform> transform_stack_;
};

}  // namespace pk
