#pragma once

#include "puck/puck_view.h"

#include <memory>
#include <vector>

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
    struct Band {
        ImageHandle image = kInvalidImage;
        int y = 0;
        int height = 0;
    };

    void draw_placeholder(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds);
    void draw_assets(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds);
    void create_bands(Canvas& canvas);
    void release_image();

    std::unique_ptr<Assets> assets_;
    Canvas* canvas_ = nullptr;
    ImageHandle image_ = kInvalidImage;
    bool image_failed_ = false;
    // The sprite is cut into horizontal bands once, so the 2D fallback can
    // lean with a shear: the Canvas API only draws axis-aligned images.
    std::vector<Band> bands_;

    float last_time_ = -1.0f;
    float gaze_x_ = 0.0f;
    float gaze_y_ = 0.0f;
    float body_turn_ = 0.0f;
    float lids_ = 0.0f;
    float mouth_open_ = 0.0f;
};

}  // namespace pk
