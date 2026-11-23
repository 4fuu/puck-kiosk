#pragma once

#include "puck/puck_view.h"

namespace pk {

class Puck3D final : public PuckView {
public:
    const char* name() const override { return "puck-3d"; }
    bool is_3d() const override { return true; }
    bool initialize(std::string* error) override;
    void draw(Canvas&, Rect, const PuckVisual&, float) override {}
};

}  // namespace pk
