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

// What the pairing screen should offer after a confirmed or failed attempt.
// Retrying is drawn as a disabled button so the app can start a fresh
// PairingSession without accepting a second tap.
enum class PairingRecovery {
    None,
    Retry,
    Retrying,
};

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

    explicit CarUi(PuckView& puck) : puck_(&puck) {}

    // The session path starts with the placeholder Puck and swaps in the real
    // renderer once the asset cache is verified.
    void set_puck(PuckView& puck) { puck_ = &puck; }

    void set_client_caption(bool enabled) { client_caption_ = enabled; }
    void set_visual(const PuckVisual& visual) { visual_ = visual; }
    void set_pairing(const QrCode& qr, std::string title, std::string status, std::string code = {});
    // The app sets this from PairingSession::restart availability. Retry is a
    // real button; Retrying draws the busy label and removes the hit target.
    void set_pairing_recovery(PairingRecovery recovery) { pairing_recovery_ = recovery; }
    PairingRecovery pairing_recovery() const { return pairing_recovery_; }
    void set_call(const CallView& view);
    // The app's CallGate result. A false value leaves the green call button
    // dimmed and absent from the hit map, so pointer and focus cannot start a
    // call until the app re-enables it.
    void set_call_available(bool available) { call_available_ = available; }
    bool call_available() const { return call_available_; }
    void set_media(MediaOverlay* media) { media_ = media; }
    void set_sheet(const PluginSheet& sheet);
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
    // The pairing status line's draw rect from the last frame. Empty when the
    // screen has no status text.
    Rect status_bounds() const { return status_bounds_; }
    // The Try Again button's draw rect from the last frame. Empty when the
    // pairing screen has no recovery action.
    Rect pairing_retry_bounds() const { return pairing_retry_bounds_; }

    // The smoothed, normalized local microphone level drawn into the Mute
    // button. Zero before the call connects, while muted, and after it ends.
    float mic_level() const { return mic_level_; }

    // The scroll affordances of a long plugin option list. Empty unless the
    // visible sheet needs scrolling. Pointer taps on them scroll one row; the
    // hit map owns the taps as SheetScroll actions, so they never reach the
    // option behind.
    Rect sheet_scroll_up() const { return sheet_scroll_up_; }
    Rect sheet_scroll_down() const { return sheet_scroll_down_; }
    bool scroll_sheet(int rows);

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
    float update_mic_level(const CallView& call, float time_seconds);
    float advance_stage_light(bool speaking, float time_seconds);

    PuckView* puck_;
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
    Rect status_bounds_;
    Rect pairing_retry_bounds_;
    PairingRecovery pairing_recovery_ = PairingRecovery::None;
    bool call_available_ = true;
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

    // Local mic meter: peak-normalized RMS with a fast attack and a slow
    // release. Muted, pre-connect, and post-call frames snap it to zero.
    float mic_level_ = 0.0f;
    float mic_peak_ = 0.0f;
    float mic_last_time_ = -1.0f;

    // The stage light follows the actor's speaking state with the same fast
    // rise and slow fall as the 3D spotlight, so a short pause does not blink.
    float stage_light_ = 0.0f;
    float stage_light_time_ = -1.0f;

    // A long option list scrolls by whole rows. Rows stay at their normal
    // height so labels never collide with Dismiss or the scroll rail.
    int sheet_scroll_row_ = 0;
    int sheet_scroll_max_ = 0;
    int sheet_scroll_capacity_ = 0;
    Rect sheet_scroll_up_;
    Rect sheet_scroll_down_;
};

}  // namespace pk
