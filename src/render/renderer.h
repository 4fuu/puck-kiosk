#pragma once

#include "render/canvas.h"

#include <memory>
#include <string>

namespace pk {

class Renderer {
public:
    bool initialize(const std::string& font_path, std::string* error);
    void begin(Size size, float dpi_scale);
    void end();
    Canvas& canvas() { return *canvas_; }

private:
    std::unique_ptr<Canvas> canvas_;
};

}  // namespace pk
