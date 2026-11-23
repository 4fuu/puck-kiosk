#pragma once

#include "app/pairing.h"
#include "core/types.h"
#include "render/canvas.h"

#include <string>

namespace pk {

class PuckView;

class CarUi {
public:
    explicit CarUi(PuckView& puck) : puck_(puck) {}

    void set_visual(const PuckVisual& visual) { visual_ = visual; }
    void set_pairing(const QrCode& qr, std::string title, std::string status, std::string code = {});
    void draw(Canvas& canvas, Size size, float time_seconds);

private:
    PuckView& puck_;
    PuckVisual visual_;
    QrCode qr_;
    std::string title_ = "Call Puck";
    std::string status_;
    std::string code_;
};

}  // namespace pk
