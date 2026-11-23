#include "app/car_ui.h"

#include "puck/puck_view.h"

#include <algorithm>
#include <cstdint>

namespace pk {
namespace {

void draw_finder(Canvas& canvas, Rect qr, int modules, int origin_x, int origin_y) {
    const float cell = qr.w / static_cast<float>(modules);
    const auto block = [&](int x, int y, int w, int h, Color color) {
        canvas.fill_rect(Rect{qr.x + x * cell, qr.y + y * cell, w * cell, h * cell}, color);
    };
    block(origin_x, origin_y, 7, 7, hex(0x0b0d0b));
    block(origin_x + 1, origin_y + 1, 5, 5, hex(0xffffff));
    block(origin_x + 2, origin_y + 2, 3, 3, hex(0x0b0d0b));
}

// M0 uses a deterministic placeholder pattern. The real QR encoder lands in
// M1 together with the pairing poll, so the layout can be checked now.
void draw_qr_placeholder(Canvas& canvas, Rect card) {
    const float radius = card.w * 0.09f;
    canvas.fill_rounded_rect(Rect{card.x - 4.0f, card.y - 4.0f, card.w + 8.0f, card.h + 8.0f}, radius + 4.0f,
                             hex(0xffffff, 0.06f));
    canvas.fill_rounded_rect(card, radius, hex(0xffffff));

    const float padding = card.w * 0.075f;
    const Rect qr{card.x + padding, card.y + padding, card.w - padding * 2.0f, card.h - padding * 2.0f};
    constexpr int modules = 25;
    const float cell = qr.w / static_cast<float>(modules);

    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            const std::uint32_t hash = (static_cast<std::uint32_t>(x) * 73856093u) ^
                                       (static_cast<std::uint32_t>(y) * 19349663u) ^ 0x9e3779b9u;
            if (((hash >> 7) & 1u) == 0u) {
                continue;
            }
            canvas.fill_rect(Rect{qr.x + x * cell, qr.y + y * cell, cell + 0.4f, cell + 0.4f}, hex(0x0b0d0b));
        }
    }

    draw_finder(canvas, qr, modules, 0, 0);
    draw_finder(canvas, qr, modules, modules - 7, 0);
    draw_finder(canvas, qr, modules, 0, modules - 7);

    const float logo = qr.w * 0.26f;
    const Rect logo_rect{qr.center_x() - logo * 0.5f, qr.center_y() - logo * 0.5f, logo, logo};
    canvas.fill_rounded_rect(logo_rect, logo * 0.22f, hex(0x0b0d0b));
    canvas.text(logo_rect.center_x(), logo_rect.center_y() + 1.0f, logo * 0.19f, hex(0xffffff),
                TextAlign::Center, "puck");
}

}  // namespace

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

    const float title_size = portrait ? 40.0f : 46.0f;
    const float title_y = panel.y + (portrait ? 64.0f : panel.h * 0.26f);
    canvas.text(panel.center_x(), title_y, title_size, foreground, TextAlign::Center, "Call Puck");

    float qr_size = std::min(panel.w * 0.52f, panel.h * 0.46f);
    qr_size = std::min(qr_size, 240.0f);
    const Rect card{panel.center_x() - qr_size * 0.5f, title_y + title_size * 0.80f, qr_size, qr_size};
    draw_qr_placeholder(canvas, card);

    canvas.text(panel.center_x(), card.y + card.h + 30.0f, 20.0f, muted, TextAlign::Center,
                "Or Sign In Manually");

    canvas.text(28.0f, 30.0f, 18.0f, muted, TextAlign::Left, "puck-kiosk");
}

}  // namespace pk
