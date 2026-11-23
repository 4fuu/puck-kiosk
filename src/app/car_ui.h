#pragma once

#include "core/types.h"
#include "render/canvas.h"

namespace pk {

class PuckView;

class CarUi {
public:
    explicit CarUi(PuckView& puck) : puck_(puck) {}

    void set_visual(const PuckVisual& visual) { visual_ = visual; }
    void draw(Canvas& canvas, Size size, float time_seconds);

private:
    PuckView& puck_;
    PuckVisual visual_;
};

}  // namespace pk
