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
    void set_call(const CallView& view);
    void draw(Canvas& canvas, Size size, float time_seconds);

private:
    void draw_pairing(Canvas& canvas, Size size, float time_seconds);
    void draw_call(Canvas& canvas, Size size, float time_seconds);

    PuckView& puck_;
    PuckVisual visual_;
    QrCode qr_;
    std::string title_ = "Call Puck";
    std::string status_;
    std::string code_;
    bool call_mode_ = false;
    CallView call_;
};

}  // namespace pk
