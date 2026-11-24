#pragma once

#include "app/diagnostics.h"
#include "app/pairing.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
#include "app/ui_focus.h"
#include "app/ui_input.h"
#include "core/types.h"
#include "render/canvas.h"

#include <chrono>
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
    void set_focus(const UiFocusView& view) { focus_ = view; }

    // Tracks whether the gamepad hint bar should be visible. Any gamepad event
    // shows it; pointer activity hides it until the next gamepad event.
    void notify_input(const InputEvent& event);
    bool hints_visible() const { return hints_visible_; }

    void draw(Canvas& canvas, Size size, float time_seconds);

    // Rects drawn by the last frame. The app hit-tests input against them.
    const UiHitMap& hit_map() const { return hits_; }

private:
    void draw_pairing(Canvas& canvas, Size size, float time_seconds);
    void draw_call(Canvas& canvas, Size size, float time_seconds);
    void draw_passkey(Canvas& canvas, Rect area, bool compact);
    void draw_sheet(Canvas& canvas, Rect area, bool compact);
    void draw_debug(Canvas& canvas, Size size);
    void draw_focus_overlay(Canvas& canvas, Size size);
    void add_debug_target(Canvas& canvas, Size size, bool pairing);
    void add_target(Rect rect, UiAction action, int index = -1);
    float control_split(float time_seconds, bool in_call);

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
    UiFocusView focus_;
    bool hints_visible_ = false;
    // The Debug entry stays invisible in normal use: hold the bottom-left
    // corner for 2s (pointer/touch) or press F12 to reveal it; gamepad Start
    // opens the panel directly. It hides again when the panel closes.
    bool debug_revealed_ = false;
    bool debug_was_open_ = false;
    bool corner_holding_ = false;
    std::chrono::steady_clock::time_point corner_hold_start_;
    Size last_size_;

    // The web page animates the single call button into Mute/End over 400ms
    // with cubic-bezier(.22, 1, .36, 1). Split is 0 before a call and 1 during
    // one; the first frame snaps instead of animating in from the wrong place.
    float control_split_ = -1.0f;
    float split_from_ = 0.0f;
    float split_start_ = 0.0f;
    float split_target_ = 0.0f;
    float last_draw_time_ = -1.0f;
};

}  // namespace pk
