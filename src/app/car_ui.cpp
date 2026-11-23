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

}  // namespace

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

    const Color foreground = hex(0xf6fff5);
    const Color muted_text = hex(0x9ca49c);
    const bool ended = call_.phase == CallPhase::Ended;
    const bool failed = call_.phase == CallPhase::Failed;
    const bool connecting =
        call_.phase == CallPhase::Starting || call_.phase == CallPhase::Connecting;

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

    draw_status_dot(canvas, width - 150.0f, 30.0f, call_.phase);
    canvas.text(width - 28.0f, 30.0f, 16.0f, muted_text, TextAlign::Right,
                ended ? "Call ended" : failed ? "Call failed" : connecting ? "Connecting" : "Connected");
    canvas.text(28.0f, 30.0f, 18.0f, muted_text, TextAlign::Left, "puck-kiosk");

    if (media_ && media_->visible()) {
        media_->draw(canvas, Rect{0.0f, 0.0f, width, stage_height});
    }

    const float controls_y = height - (portrait ? 116.0f : 108.0f);
    const float button_height = portrait ? 76.0f : 72.0f;
    const float button_width = std::min(width * 0.34f, 260.0f);
    const float gap = 24.0f;

    if (ended || failed) {
        const float pill_width = std::min(width * 0.42f, 320.0f);
        const Rect pill{width * 0.5f - pill_width * 0.5f, controls_y - button_height * 0.5f,
                        pill_width, button_height};
        if (failed) {
            draw_pill(canvas, pill, hex(0xf0f4ef), hex(0x121712), "Try Again", 26.0f);
        } else {
            draw_pill(canvas, pill, hex(0x5fd08a), hex(0x0b160e), "Call Puck", 26.0f);
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
        draw_pill(canvas, end_button, hex(0xd6453f), hex(0xfff7f6), "End", 26.0f);
    }
}

}  // namespace pk
