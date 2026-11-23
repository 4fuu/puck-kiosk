#include "app/car_ui.h"

#include "puck/puck_view.h"

#include <algorithm>
#include <cstdint>

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

}  // namespace

void CarUi::set_pairing(const QrCode& qr, std::string title, std::string status, std::string code) {
    qr_ = qr;
    title_ = std::move(title);
    status_ = std::move(status);
    code_ = std::move(code);
}

void CarUi::draw(Canvas& canvas, Size size, float time_seconds) {
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

}  // namespace pk
