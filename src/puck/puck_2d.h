#pragma once

#include "puck/puck_view.h"

namespace pk {

class Puck2D final : public PuckView {
public:
    const char* name() const override { return "puck-2d"; }
    bool is_3d() const override { return false; }
    bool initialize(std::string*) override { return true; }
    void draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) override;
};

}  // namespace pk
