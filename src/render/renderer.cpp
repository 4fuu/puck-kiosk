#include "render/renderer.h"

#include "render/nanovg_canvas.h"

#include <memory>

namespace pk {

bool Renderer::initialize(const std::string& font_path, std::string* error) {
    auto canvas = std::make_unique<NanoVgCanvas>();
    if (!canvas->initialize(font_path, error)) {
        return false;
    }
    canvas_ = std::move(canvas);
    return true;
}

void Renderer::begin(Size size, float dpi_scale) {
    canvas_->begin_frame(size, dpi_scale);
    canvas_->clear(hex(0x0b0d0b));
}

void Renderer::end() {
    canvas_->end_frame();
}

}  // namespace pk
