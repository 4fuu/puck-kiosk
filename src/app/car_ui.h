#pragma once

#include "app/diagnostics.h"
#include "app/pairing.h"
#include "app/passkey.h"
#include "app/plugin_ui.h"
#include "app/ui_focus.h"
#include "app/ui_input.h"
#include "core/types.h"
#include "render/canvas.h"

#include <string>

namespace pk {

class MediaOverlay;
class PuckView;

class CarUi {
public:
    // The client caption strip the app draws when the window has no OS title
    // bar. It is chrome: the screen content is laid out in the area below it.
    static constexpr float kCaptionHeight = 36.0f;
    static constexpr float kCaptionButtonWidth = 40.0f;
    static constexpr float kCaptionButtonHeight = 28.0f;
    static constexpr float kCaptionRightMargin = 4.0f;
    static constexpr float kCaptionButtonsWidth =
        kCaptionButtonWidth * 3.0f + kCaptionRightMargin;

    explicit CarUi(PuckView& puck) : puck_(puck) {}

    void set_client_caption(bool enabled) { client_caption_ = enabled; }
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

    // The Puck's draw rect and the stage it may be dragged across. Empty when
    // the Puck is not on screen (a sheet or the debug panel is covering it).
    Rect puck_bounds() const { return puck_bounds_; }
    Rect puck_stage() const { return puck_stage_; }

private:
    void draw_pairing(Canvas& canvas, Size size, float time_seconds);
    void draw_call(Canvas& canvas, Size size, float time_seconds);
    void draw_passkey(Canvas& canvas, Rect area, bool compact);
    void draw_sheet(Canvas& canvas, Rect area, bool compact);
    void draw_debug(Canvas& canvas, Size size);
    void draw_focus_overlay(Canvas& canvas, Size size);
    void draw_client_caption(Canvas& canvas, Size size);
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
    Rect puck_bounds_;
    Rect puck_stage_;
    UiFocusView focus_;
    bool hints_visible_ = false;
    // Windowed Win32 has no OS title bar; the app draws this strip and the
    // display reports pointer input in the content area below it.
    bool client_caption_ = false;
    // The Debug entry stays invisible in normal use: hold the bottom-left
    // corner for 2s or tap it 5 times within 2.5s (pointer/touch), press F12,
    // or hold Back+Start on a gamepad for 1.5s. It hides again when the panel
    // closes.
    bool debug_revealed_ = false;
    bool debug_was_open_ = false;
    bool corner_holding_ = false;
    float corner_hold_start_ = 0.0f;
    int corner_taps_ = 0;
    float tap_window_start_ = -1.0f;
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
