#pragma once

#include "platform/display.h"

#include <string>

namespace pk {

class HeadlessDisplay final : public Display {
public:
    HeadlessDisplay(int width, int height, std::string output_path, int frames);
    ~HeadlessDisplay() override;

    bool initialize(std::string* error) override;
    Size size() const override { return size_; }
    float dpi_scale() const override { return 1.0f; }
    void make_current() override;
    void swap() override;
    bool should_close() const override { return frame_ >= frames_; }
    const char* backend_name() const override { return "headless"; }

private:
    bool write_ppm(const std::string& path, std::string* error) const;
    void destroy();

    Size size_;
    std::string output_path_;
    int frames_ = 1;
    int frame_ = 0;

    void* egl_display_ = nullptr;
    void* egl_context_ = nullptr;
    void* egl_config_ = nullptr;
    unsigned int framebuffer_ = 0;
    unsigned int color_texture_ = 0;
    unsigned int stencil_buffer_ = 0;
};

class HeadlessInput final : public InputSource {
public:
    bool poll(InputEvent&) override { return false; }
};

}  // namespace pk
