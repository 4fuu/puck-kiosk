#include "app/car_ui.h"

#include "app/media_overlay.h"
#include "puck/puck_view.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pk {
namespace {

constexpr int kQuietZoneModules = 4;

void draw_qr(Canvas& canvas, Rect card, const QrCode& qr) {
    const float radius = card.w * 0.09f;
    canvas.fill_rounded_rect(Rect{card.x - 4.0f, card.y - 4.0f, card.w + 8.0f, card.h + 8.0f},
                             radius + 4.0f, hex(0xffffff, 0.06f));
    canvas.fill_rounded_rect(card, radius, hex(0xffffff));

    const int total_modules = qr.size + kQuietZoneModules * 2;
    const float cell = std::min(card.w, card.h) / static_cast<float>(total_modules + 2);
    const float extent = cell * static_cast<float>(total_modules);
    const float origin_x = card.center_x() - extent * 0.5f;
    const float origin_y = card.center_y() - extent * 0.5f;
    const Color dark = hex(0x0b0d0b);

    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.dark(x, y)) {
                continue;
            }
            canvas.fill_rect(Rect{origin_x + (x + kQuietZoneModules) * cell,
                                  origin_y + (y + kQuietZoneModules) * cell, cell + 0.4f, cell + 0.4f},
                             dark);
        }
    }
}

// Normal render mode has no pairing challenge, so show an honest placeholder
// instead of a QR-shaped pattern that cannot be scanned.
void draw_placeholder(Canvas& canvas, Rect card) {
    const float radius = card.w * 0.09f;
    canvas.fill_rounded_rect(Rect{card.x - 4.0f, card.y - 4.0f, card.w + 8.0f, card.h + 8.0f},
                             radius + 4.0f, hex(0xffffff, 0.06f));
    canvas.fill_rounded_rect(card, radius, hex(0xffffff));

    const float box = card.w * 0.32f;
    const Rect inner{card.center_x() - box * 0.5f, card.center_y() - box * 0.5f, box, box};
    canvas.fill_rounded_rect(inner, box * 0.12f, hex(0x0b0d0b, 0.08f));
    canvas.text(inner.center_x(), inner.center_y() + 1.0f, box * 0.22f, hex(0x9ca49c),
                TextAlign::Center, "QR");
    canvas.text(card.center_x(), card.y + card.h - 26.0f, 16.0f, hex(0x9ca49c), TextAlign::Center,
                "run with --pair");
}

void draw_wrapped_text(Canvas& canvas, float center_x, float top, float size, Color color,
                       const std::string& text, float max_width, int max_lines) {
    if (text.empty() || max_lines <= 0) {
        return;
    }
    std::vector<std::string> lines;
    std::string line;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t space = text.find(' ', index);
        const std::string word =
            text.substr(index, space == std::string::npos ? std::string::npos : space - index);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && canvas.measure_text(size, candidate) > max_width) {
            lines.push_back(line);
            line = word;
            if (static_cast<int>(lines.size()) >= max_lines) {
                break;
            }
        } else {
            line = candidate;
        }
        if (space == std::string::npos) {
            break;
        }
        index = space + 1;
    }
    if (static_cast<int>(lines.size()) < max_lines && !line.empty()) {
        lines.push_back(line);
    }

    float y = top + size;
    for (const std::string& current : lines) {
        canvas.text(center_x, y, size, color, TextAlign::Center, current);
        y += size * 1.24f;
    }
}

std::string fit_text(Canvas& canvas, float size, const std::string& text, float max_width) {
    if (max_width <= 0.0f || canvas.measure_text(size, text) <= max_width) {
        return text;
    }
    std::string result = text;
    while (!result.empty() && canvas.measure_text(size, result + "\u2026") > max_width) {
        result.pop_back();
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) {
            result.pop_back();
        }
    }
    return result.empty() ? std::string{} : result + "\u2026";
}

void draw_status_dot(Canvas& canvas, float x, float y, CallPhase phase) {
    Color color = hex(0x6b746b);
    if (phase == CallPhase::Active || phase == CallPhase::Speaking ||
        phase == CallPhase::Thinking || phase == CallPhase::Muted) {
        color = hex(0x5fd08a);
    } else if (phase == CallPhase::Starting || phase == CallPhase::Connecting) {
        color = hex(0xe0b85f);
    } else if (phase == CallPhase::Failed) {
        color = hex(0xe0524b);
    }
    canvas.fill_circle(x, y, 6.0f, color);
}

void draw_pill(Canvas& canvas, Rect rect, Color fill, Color text_color, const std::string& label,
               float font_size) {
    canvas.fill_rounded_rect(rect, rect.h * 0.5f, fill);
    canvas.text(rect.center_x(), rect.center_y() + font_size * 0.36f, font_size, text_color,
                TextAlign::Center, label);
}

void draw_phone_icon(Canvas& canvas, float center_x, float center_y, float radius, Color color) {
    canvas.fill_circle(center_x, center_y, radius, hex(0x232b23));
    const float top = center_y - radius * 0.36f;
    const float width = radius * 1.16f;
    const float bar = radius * 0.22f;
    const float left = center_x - width * 0.5f;
    canvas.fill_rounded_rect(Rect{left, top, width, bar}, bar * 0.5f, color);
    canvas.fill_rounded_rect(Rect{left, top, bar, radius * 0.72f}, bar * 0.5f, color);
    canvas.fill_rounded_rect(Rect{left + width - bar, top, bar, radius * 0.72f}, bar * 0.5f, color);
}

}  // namespace

void CarUi::add_target(Rect rect, UiAction action, int index) {
    hits_.add(rect, action, index);
}

void CarUi::set_pairing(const QrCode& qr, std::string title, std::string status, std::string code) {
    call_mode_ = false;
    qr_ = qr;
    title_ = std::move(title);
    status_ = std::move(status);
    code_ = std::move(code);
}

void CarUi::set_call(const CallView& view) {
    call_mode_ = true;
    call_ = view;
}

void CarUi::draw(Canvas& canvas, Size size, float time_seconds) {
    hits_.clear();
    if (debug_.open) {
        draw_debug(canvas, size);
        return;
    }
    if (call_mode_) {
        draw_call(canvas, size, time_seconds);
    } else {
        draw_pairing(canvas, size, time_seconds);
    }
}

void CarUi::draw_pairing(Canvas& canvas, Size size, float time_seconds) {
    const bool portrait = size.height > size.width;
    Rect puck_area;
    Rect panel;
    if (portrait) {
        puck_area = Rect{0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height) * 0.58f};
        panel = Rect{0.0f, puck_area.h, static_cast<float>(size.width),
                     static_cast<float>(size.height) - puck_area.h};
    } else {
        puck_area = Rect{0.0f, 0.0f, static_cast<float>(size.width) * 0.60f, static_cast<float>(size.height)};
        panel = Rect{puck_area.w, 0.0f, static_cast<float>(size.width) - puck_area.w,
                     static_cast<float>(size.height)};
    }

    // A faint studio glow behind Puck; the final 3D renderer draws a real
    // spotlight, but the 2D fallback should still feel like the same stage.
    canvas.fill_ellipse(puck_area.center_x(), puck_area.center_y() + puck_area.h * 0.14f,
                        puck_area.w * 0.34f, puck_area.h * 0.40f, hex(0x121712, 0.75f));
    canvas.fill_ellipse(puck_area.center_x(), puck_area.center_y() + puck_area.h * 0.08f,
                        puck_area.w * 0.24f, puck_area.h * 0.28f, hex(0x171d15, 0.55f));

    puck_.draw(canvas, puck_area, visual_, time_seconds);

    const Color foreground = hex(0xf6fff5);
    const Color muted = hex(0x9ca49c);

    float title_size = portrait ? 40.0f : 46.0f;
    while (title_size > 22.0f && canvas.measure_text(title_size, title_) > panel.w * 0.9f) {
        title_size -= 2.0f;
    }
    const float title_y = panel.y + (portrait ? 64.0f : panel.h * 0.26f);
    canvas.text(panel.center_x(), title_y, title_size, foreground, TextAlign::Center, title_);

    float qr_size = std::min(panel.w * 0.52f, panel.h * 0.46f);
    qr_size = std::min(qr_size, 300.0f);
    const Rect card{panel.center_x() - qr_size * 0.5f, title_y + title_size * 0.80f, qr_size, qr_size};

    if (qr_.empty()) {
        draw_placeholder(canvas, card);
    } else {
        draw_qr(canvas, card, qr_);
    }

    const float below_card = card.y + card.h;
    if (!code_.empty()) {
        canvas.text(panel.center_x(), below_card + 26.0f, 20.0f, muted, TextAlign::Center, status_);
        canvas.text(panel.center_x(), below_card + 68.0f, 44.0f, foreground, TextAlign::Center, code_);
    } else if (!status_.empty()) {
        canvas.text(panel.center_x(), below_card + 30.0f, 20.0f, muted, TextAlign::Center, status_);
    } else {
        canvas.text(panel.center_x(), below_card + 30.0f, 20.0f, muted, TextAlign::Center,
                    "Or Sign In Manually");
    }

    canvas.text(28.0f, 30.0f, 18.0f, muted, TextAlign::Left, "puck-kiosk");
    draw_debug_entry(canvas, size);
}

void CarUi::draw_call(Canvas& canvas, Size size, float time_seconds) {
    const bool portrait = size.height > size.width;
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);

    const float stage_height = height * (portrait ? 0.68f : 0.72f);
    Rect puck_area{0.0f, 0.0f, width, stage_height};
    const float puck_size = std::min(puck_area.w * (portrait ? 0.82f : 0.40f),
                                     puck_area.h * (portrait ? 0.70f : 0.70f));
    const Rect puck_rect{puck_area.center_x() - puck_size * 0.5f,
                         puck_area.center_y() - puck_size * 0.62f, puck_size, puck_size};

    const Color foreground = hex(0xf6fff5);
    const Color muted_text = hex(0x9ca49c);
    const bool ended = call_.phase == CallPhase::Ended;
    const bool failed = call_.phase == CallPhase::Failed;
    const bool connecting =
        call_.phase == CallPhase::Starting || call_.phase == CallPhase::Connecting;
    const bool in_call = call_.in_call();

    if (passkey_.mode != PasskeyMode::Hidden && !in_call) {
        draw_passkey(canvas, puck_area);
    } else {
        canvas.fill_ellipse(puck_area.center_x(), puck_rect.center_y() + puck_size * 0.26f,
                            puck_size * 0.48f, puck_size * 0.38f, hex(0x141a14, 0.8f));
        canvas.fill_ellipse(puck_area.center_x(), puck_rect.center_y() + puck_size * 0.18f,
                            puck_size * 0.32f, puck_size * 0.26f, hex(0x1a211a, 0.55f));

        PuckVisual visual = visual_;
        visual.speaking = call_.speaking;
        visual.listening = call_.phase == CallPhase::Active && !call_.muted;
        visual.thinking = call_.thinking;
        visual.not_listening = call_.muted;
        visual.mouth = call_.mouth;
        puck_.draw(canvas, puck_rect, visual, time_seconds);

        float caption_size = portrait ? 36.0f : 44.0f;
        while (caption_size > 22.0f && canvas.measure_text(caption_size, call_.caption) > width * 0.84f) {
            caption_size -= 2.0f;
        }
        const float caption_top = stage_height - (portrait ? 100.0f : 72.0f);
        canvas.text(width * 0.5f, caption_top, caption_size, foreground, TextAlign::Center,
                    call_.caption);

        if (!call_.detail.empty()) {
            draw_wrapped_text(canvas, width * 0.5f, caption_top + caption_size * 0.9f,
                              portrait ? 18.0f : 20.0f, muted_text, call_.detail, width * 0.78f, 2);
        } else if (connecting) {
            canvas.text(width * 0.5f, caption_top + caption_size * 0.9f, 18.0f, muted_text,
                        TextAlign::Center, "Setting up the voice channel");
        }
    }

    draw_status_dot(canvas, width - 150.0f, 30.0f, call_.phase);
    canvas.text(width - 28.0f, 30.0f, 16.0f, muted_text, TextAlign::Right,
                ended ? "Call ended" : failed ? "Call failed" : connecting ? "Connecting" : "Connected");
    canvas.text(28.0f, 30.0f, 18.0f, muted_text, TextAlign::Left, "puck-kiosk");

    if (media_ && media_->visible()) {
        media_->draw(canvas, puck_area);
        const MediaButtons buttons = media_->buttons(canvas, puck_area);
        add_target(buttons.close, UiAction::MediaClose);
        if (buttons.can_previous) {
            add_target(buttons.previous, UiAction::MediaPrevious);
        }
        if (buttons.can_next) {
            add_target(buttons.next, UiAction::MediaNext);
        }
    }

    if (sheet_.kind != PluginSheetKind::None) {
        draw_sheet(canvas, puck_area);
    }

    const float controls_y = height - (portrait ? 116.0f : 108.0f);
    const float button_height = portrait ? 76.0f : 72.0f;
    const float button_width = std::min(width * 0.34f, 260.0f);
    const float gap = 24.0f;

    const bool passkey_button = (passkey_.confirm_button || passkey_.confirming) && !in_call;
    if (passkey_button) {
        const float pill_width = std::min(width * 0.46f, 420.0f);
        const Rect pill{width * 0.5f - pill_width * 0.5f, controls_y - button_height * 0.5f,
                        pill_width, button_height};
        if (passkey_.confirming) {
            draw_pill(canvas, pill, hex(0x2a332a), hex(0x9ca49c), "Confirming\u2026", 26.0f);
        } else {
            draw_pill(canvas, pill, hex(0x5fd08a), hex(0x0b160e), "Confirm Passkey", 26.0f);
            add_target(pill, UiAction::PasskeyConfirm);
        }
    } else if (!in_call) {
        const float pill_width = std::min(width * 0.42f, 320.0f);
        const Rect pill{width * 0.5f - pill_width * 0.5f, controls_y - button_height * 0.5f,
                        pill_width, button_height};
        if (passkey_.mode == PasskeyMode::Phone) {
            // The actor is read-only until the passkey is confirmed on the phone.
            draw_pill(canvas, pill, hex(0x1f3a28), hex(0x6f8a75), "Call Puck", 26.0f);
        } else {
            draw_pill(canvas, pill, hex(0x5fd08a), hex(0x0b160e),
                      failed ? "Try Again" : "Call Puck", 26.0f);
            add_target(pill, UiAction::Call);
        }
    } else {
        const float total_width = button_width * 2.0f + gap;
        const float left = width * 0.5f - total_width * 0.5f;
        const Rect mute_button{left, controls_y - button_height * 0.5f, button_width, button_height};
        const Rect end_button{left + button_width + gap, controls_y - button_height * 0.5f,
                              button_width, button_height};
        if (call_.muted) {
            draw_pill(canvas, mute_button, hex(0xd9e6d8), hex(0x121712), "Unmute", 26.0f);
        } else {
            draw_pill(canvas, mute_button, hex(0x232b23), foreground, "Mute", 26.0f);
        }
        if (!connecting && !failed) {
            add_target(mute_button, UiAction::Mute);
        }
        draw_pill(canvas, end_button, hex(0xd6453f), hex(0xfff7f6), "End", 26.0f);
        add_target(end_button, UiAction::End);
    }

    draw_debug_entry(canvas, size);
}

void CarUi::draw_passkey(Canvas& canvas, Rect stage) {
    const Color foreground = hex(0xf6fff5);
    const Color muted = hex(0x9ca49c);
    const Color secondary = hex(0x232b23);

    const float content_width = std::min(stage.w * 0.82f, 900.0f);
    const float left = stage.center_x() - content_width * 0.5f;
    float y = stage.y + (stage.h < 420.0f ? 34.0f : 52.0f);

    const float title_size = stage.h < 420.0f ? 34.0f : 44.0f;
    canvas.text(stage.center_x(), y + title_size * 0.8f, title_size, foreground,
                TextAlign::Center, fit_text(canvas, title_size, passkey_.title, content_width));
    y += title_size + 34.0f;

    for (std::size_t i = 0; i < passkey_.steps.size(); ++i) {
        const PasskeyStep& step = passkey_.steps[i];
        const float number_radius = stage.h < 420.0f ? 20.0f : 26.0f;
        const float number_x = left + number_radius;
        const float number_y = y + number_radius;
        canvas.fill_circle(number_x, number_y, number_radius, secondary);
        canvas.text(number_x, number_y + 8.0f, number_radius * 0.9f, foreground,
                    TextAlign::Center, std::to_string(i + 1));

        const float text_left = left + number_radius * 2.0f + 20.0f;
        const float step_size = stage.h < 420.0f ? 25.0f : 32.0f;
        const float detail_size = stage.h < 420.0f ? 17.0f : 20.0f;
        canvas.text(text_left, y + number_radius + 2.0f, step_size, foreground, TextAlign::Left,
                    fit_text(canvas, step_size, step.title, stage.w - text_left - 24.0f));
        if (!step.detail.empty()) {
            canvas.text(text_left, y + number_radius + step_size + 14.0f, detail_size, muted,
                        TextAlign::Left,
                        fit_text(canvas, detail_size, step.detail, stage.w - text_left - 24.0f));
        }
        y += number_radius * 2.0f + (step.detail.empty() ? 16.0f : 40.0f);
    }

    if (!passkey_.status.empty()) {
        canvas.text(stage.center_x(), stage.y + stage.h - 18.0f, 17.0f,
                    passkey_.mode == PasskeyMode::Failed ? hex(0xe0524b) : muted,
                    TextAlign::Center, passkey_.status);
    }
}

void CarUi::draw_sheet(Canvas& canvas, Rect stage) {
    add_target(stage, UiAction::None);
    canvas.fill_rect(stage, hex(0x0b0d0b));

    const Color foreground = hex(0xf6fff5);
    const Color muted = hex(0x9ca49c);
    const Color secondary = hex(0x232b23);
    const float content_width = std::min(stage.w * 0.78f, 900.0f);
    const float left = stage.center_x() - content_width * 0.5f;

    if (sheet_.kind == PluginSheetKind::Phone) {
        const float center_y = stage.center_y();
        draw_phone_icon(canvas, stage.center_x(), center_y - 96.0f, 42.0f, foreground);
        canvas.text(stage.center_x(), center_y - 18.0f, 34.0f, foreground, TextAlign::Center,
                    "Answer on Your Phone");
        canvas.text(stage.center_x(), center_y + 18.0f, 21.0f, muted, TextAlign::Center,
                    fit_text(canvas, 21.0f, sheet_.title, content_width));
        const float ok_width = std::min(240.0f, content_width);
        const Rect ok{stage.center_x() - ok_width * 0.5f, center_y + 56.0f, ok_width, 62.0f};
        draw_pill(canvas, ok, secondary, foreground, "OK", 24.0f);
        add_target(ok, UiAction::SheetOk);
        return;
    }

    const bool confirm = sheet_.kind == PluginSheetKind::Confirm;
    const std::size_t count = confirm ? 2 : sheet_.options.size();
    const bool dense = count > 4;
    const float option_height = dense ? 60.0f : (stage.h < 420.0f ? 62.0f : 84.0f);
    const float gap = dense ? 12.0f : 16.0f;
    const int columns = 2;
    const int rows = static_cast<int>((count + columns - 1) / columns);
    const float dismiss_height = 58.0f;
    const float title_size = 34.0f;
    // The car page shows the request title as a small line above the message,
    // and promotes the message to the heading when one is present.
    const bool has_eyebrow = !sheet_.message.empty();
    const std::string eyebrow = has_eyebrow ? sheet_.title : std::string{};
    const std::string heading = has_eyebrow ? sheet_.message : sheet_.title;
    const float grid_height = rows > 0 ? rows * option_height + (rows - 1) * gap : 0.0f;
    const float total_height = (has_eyebrow ? 26.0f : 0.0f) + title_size + 22.0f + 14.0f +
                               grid_height + 22.0f + dismiss_height;
    float y = stage.y + std::max(16.0f, (stage.h - total_height) * 0.5f);
    if (has_eyebrow) {
        canvas.text(stage.center_x(), y + 14.0f, 19.0f, muted, TextAlign::Center,
                    fit_text(canvas, 19.0f, eyebrow, content_width));
        y += 26.0f;
    }
    canvas.text(stage.center_x(), y + title_size * 0.8f, title_size, foreground, TextAlign::Center,
                fit_text(canvas, title_size, heading, content_width));
    y += title_size + 22.0f;
    y += 14.0f;

    const float dismiss_limit = stage.y + stage.h - 16.0f - dismiss_height;
    const float available = dismiss_limit - 12.0f - y;
    float dismiss_top = y + grid_height + 22.0f;
    if (grid_height > available && rows > 0) {
        // Our canvas has no scrolling; compress the grid so Dismiss stays on
        // screen for unusually long option lists.
        const float compressed = std::max(44.0f, (available - (rows - 1) * gap) / rows);
        const float button_width = (content_width - gap * (columns - 1)) / columns;
        for (std::size_t i = 0; i < count; ++i) {
            const int column = static_cast<int>(i) % columns;
            const int row = static_cast<int>(i) / columns;
            const Rect button{left + column * (button_width + gap), y + row * (compressed + gap),
                              button_width, compressed};
            const std::string label = confirm ? (i == 0 ? sheet_.confirm_label : "No")
                                              : sheet_.options[i];
            draw_pill(canvas, button, secondary, foreground, fit_text(canvas, 22.0f, label, button.w - 24.0f),
                      22.0f);
            add_target(button, UiAction::SheetOption, static_cast<int>(i));
        }
        dismiss_top = y + rows * compressed + (rows - 1) * gap + 22.0f;
    } else {
        const float button_width = (content_width - gap * (columns - 1)) / columns;
        for (std::size_t i = 0; i < count; ++i) {
            const int column = static_cast<int>(i) % columns;
            const int row = static_cast<int>(i) / columns;
            const Rect button{left + column * (button_width + gap), y + row * (option_height + gap),
                              button_width, option_height};
            const std::string label = confirm ? (i == 0 ? sheet_.confirm_label : "No")
                                              : sheet_.options[i];
            const float font = dense ? 19.0f : (stage.h < 420.0f ? 21.0f : 25.0f);
            canvas.fill_rounded_rect(button, 16.0f, secondary);
            canvas.text(button.center_x(), button.center_y() + font * 0.36f, font, foreground,
                        TextAlign::Center, fit_text(canvas, font, label, button.w - 24.0f));
            add_target(button, UiAction::SheetOption, static_cast<int>(i));
        }
    }

    const float dismiss_width = std::min(220.0f, content_width);
    dismiss_top = std::min(dismiss_top, dismiss_limit);
    const Rect dismiss{stage.center_x() - dismiss_width * 0.5f, dismiss_top, dismiss_width,
                       dismiss_height};
    canvas.text(dismiss.center_x(), dismiss.center_y() + 8.0f, 21.0f, muted, TextAlign::Center,
                "Dismiss");
    add_target(dismiss, UiAction::SheetDismiss);
}

void CarUi::draw_debug(Canvas& canvas, Size size) {
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    const Color foreground = hex(0xf6fff5);
    const Color muted = hex(0x9ca49c);
    const Color secondary = hex(0x232b23);
    const Color selected = hex(0xf0f4ef);

    canvas.fill_rect(Rect{0.0f, 0.0f, width, height}, hex(0x0b0d0b));
    canvas.text(40.0f, 52.0f, 30.0f, foreground, TextAlign::Left, "Debug");

    const Rect close{width - 180.0f, 20.0f, 140.0f, 64.0f};
    draw_pill(canvas, close, secondary, foreground, "Close", 22.0f);
    add_target(close, UiAction::DebugClose);

    const bool narrow = width < 1100.0f;
    float actions_y = 100.0f;
    canvas.text(40.0f, actions_y + 36.0f, 19.0f, muted, TextAlign::Left, "Echo Cancellation");

    struct ModePill {
        const char* label;
        EchoCancellationMode mode;
        float width;
    };
    const ModePill modes[] = {
        {"On", EchoCancellationMode::On, 90.0f},
        {"Remote Only", EchoCancellationMode::RemoteOnly, 170.0f},
        {"Off", EchoCancellationMode::Off, 90.0f},
    };
    float x = 230.0f;
    for (int i = 0; i < 3; ++i) {
        const Rect pill{x, actions_y, modes[i].width, 56.0f};
        const bool active = debug_.echo == modes[i].mode;
        draw_pill(canvas, pill, active ? selected : secondary, active ? hex(0x121712) : foreground,
                  modes[i].label, 19.0f);
        add_target(pill, UiAction::EchoMode, i);
        x += modes[i].width + 12.0f;
    }

    float buttons_y = actions_y;
    if (narrow) {
        actions_y += 72.0f;
        buttons_y = actions_y;
        x = 40.0f;
    }
    const Rect mic{x, buttons_y, 210.0f, 56.0f};
    if (debug_.mic_busy) {
        draw_pill(canvas, mic, hex(0x1a211a), hex(0x6b746b), "Test Microphone", 19.0f);
    } else {
        draw_pill(canvas, mic, secondary, foreground, "Test Microphone", 19.0f);
        add_target(mic, UiAction::MicTest);
    }
    const Rect send{x + 226.0f, buttons_y, 220.0f, 56.0f};
    if (debug_.can_send) {
        draw_pill(canvas, send, secondary, foreground, "Send Diagnostics", 19.0f);
        add_target(send, UiAction::SendDiagnostics);
    } else {
        draw_pill(canvas, send, hex(0x1a211a), hex(0x6b746b), "Send Diagnostics", 19.0f);
    }
    const std::string status =
        !debug_.can_send ? "Sign in to send"
        : debug_.send_status.empty() ? "Echo setting applies to the next call"
                                     : debug_.send_status;
    const float status_y = narrow ? buttons_y + 78.0f : actions_y + 78.0f;
    canvas.text(40.0f, status_y, 17.0f, muted, TextAlign::Left,
                fit_text(canvas, 17.0f, status, width - 80.0f));

    float row_y = status_y + 30.0f;
    const float row_height = 28.0f;
    const int max_rows = std::max(0, static_cast<int>((height - row_y - 16.0f) / row_height));
    const int rows = static_cast<int>(debug_.rows.size());
    const int shown = std::min(rows, max_rows);
    for (int i = 0; i < shown; ++i) {
        const DebugRow& row = debug_.rows[static_cast<std::size_t>(i)];
        canvas.text(40.0f, row_y + 18.0f, 18.0f, muted, TextAlign::Left, row.label);
        canvas.text(250.0f, row_y + 18.0f, 18.0f, foreground, TextAlign::Left,
                    fit_text(canvas, 18.0f, row.value, width - 290.0f));
        row_y += row_height;
    }
    if (shown < rows) {
        canvas.text(40.0f, row_y + 18.0f, 16.0f, muted, TextAlign::Left,
                    "+" + std::to_string(rows - shown) + " more");
    }
}

void CarUi::draw_debug_entry(Canvas& canvas, Size size) {
    const Rect entry{20.0f, static_cast<float>(size.height) - 62.0f, 116.0f, 42.0f};
    canvas.fill_rounded_rect(entry, 21.0f, hex(0xffffff, 0.05f));
    canvas.text(entry.center_x(), entry.center_y() + 6.0f, 16.0f, hex(0xf6fff5, 0.28f),
                TextAlign::Center, "Debug");
    add_target(entry, UiAction::DebugOpen);
}

}  // namespace pk
