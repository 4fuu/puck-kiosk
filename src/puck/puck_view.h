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
    virtual void draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) = 0;
};

std::unique_ptr<PuckView> create_puck_view(bool enable_3d);

}  // namespace pk
