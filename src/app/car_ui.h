#pragma once

#include "app/diagnostics.h"
#include "app/pairing.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
#include "app/ui_input.h"
#include "core/types.h"
#include "render/canvas.h"

#include <string>

namespace pk {

class MediaOverlay;
class PuckView;

class CarUi {
public:
    explicit CarUi(PuckView& puck) : puck_(puck) {}

    void set_visual(const PuckVisual& visual) { visual_ = visual; }
    void set_pairing(const QrCode& qr, std::string title, std::string status, std::string code = {});
    void set_call(const CallView& view);
    void set_media(MediaOverlay* media) { media_ = media; }
    void set_sheet(const PluginSheet& sheet) { sheet_ = sheet; }
    void set_passkey(const PasskeyView& view) { passkey_ = view; }
    void set_debug(const DebugView& view) { debug_ = view; }

    void draw(Canvas& canvas, Size size, float time_seconds);

    // Rects drawn by the last frame. The app hit-tests input against them.
    const UiHitMap& hit_map() const { return hits_; }

private:
    void draw_pairing(Canvas& canvas, Size size, float time_seconds);
    void draw_call(Canvas& canvas, Size size, float time_seconds);
    void draw_passkey(Canvas& canvas, Rect stage);
    void draw_sheet(Canvas& canvas, Rect stage);
    void draw_debug(Canvas& canvas, Size size);
    void draw_debug_entry(Canvas& canvas, Size size);
    void add_target(Rect rect, UiAction action, int index = -1);

    PuckView& puck_;
    MediaOverlay* media_ = nullptr;
    PuckVisual visual_;
    QrCode qr_;
    std::string title_ = "Call Puck";
    std::string status_;
    std::string code_;
    bool call_mode_ = false;
    CallView call_;
    PluginSheet sheet_;
    PasskeyView passkey_;
    DebugView debug_;
    UiHitMap hits_;
};

}  // namespace pk
