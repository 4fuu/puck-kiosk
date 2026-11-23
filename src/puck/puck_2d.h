#pragma once

#include "puck/puck_view.h"

#include <memory>

namespace pk {

class Puck2D final : public PuckView {
public:
    Puck2D();
    ~Puck2D() override;

    const char* name() const override { return "puck-2d"; }
    bool is_3d() const override { return false; }
    bool initialize(std::string*) override;
    void draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) override;

private:
    struct Assets;

    void draw_placeholder(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds);
    void draw_assets(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds);
    void release_image();

    std::unique_ptr<Assets> assets_;
    Canvas* canvas_ = nullptr;
    ImageHandle image_ = kInvalidImage;
    bool image_failed_ = false;

    float last_time_ = -1.0f;
    float gaze_x_ = 0.0f;
    float gaze_y_ = 0.0f;
    float lids_ = 0.0f;
    float mouth_open_ = 0.0f;
};

}  // namespace pk
