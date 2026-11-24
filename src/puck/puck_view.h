#pragma once

#include "core/types.h"
#include "render/canvas.h"

#include <memory>
#include <string>

namespace pk {

// The visual seam between the app and the Puck renderer. The 3D renderer and
// the 2D renderer implement the same interface, so the app can fall back at
// runtime exactly like the web client does when WebGL2 is unavailable.
class PuckView {
public:
    virtual ~PuckView() = default;
    virtual const char* name() const = 0;
    virtual bool is_3d() const = 0;
    virtual bool initialize(std::string* error) = 0;
    // The area the Puck may be dragged across. Puck3D sizes its offscreen
    // target to it so a grabbed Puck is not clipped at its layout rect; the
    // 2D renderer draws directly and ignores it.
    virtual void set_stage(Rect stage) { (void)stage; }
    virtual void draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) = 0;
};

std::unique_ptr<PuckView> create_puck_view(bool enable_3d);

}  // namespace pk
