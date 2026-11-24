#pragma once

#include "puck/puck_view.h"

#include <memory>

namespace pk {

// The optional GLES 3.0 / desktop GL 3.3 PUCK v2 renderer. initialize() loads
// and uploads the classic avatar, and any failure makes create_puck_view() fall
// back to Puck2D.
class Puck3D final : public PuckView {
public:
    Puck3D();
    ~Puck3D() override;
    Puck3D(const Puck3D&) = delete;
    Puck3D& operator=(const Puck3D&) = delete;

    const char* name() const override { return "puck-3d"; }
    bool is_3d() const override { return true; }
    bool initialize(std::string* error) override;
    void set_stage(Rect stage) override;
    void draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pk
