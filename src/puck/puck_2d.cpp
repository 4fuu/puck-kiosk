#include "puck/puck_2d.h"

#include <algorithm>
#include <cmath>

namespace pk {

void Puck2D::draw(Canvas& canvas, Rect bounds, const PuckVisual& visual, float time_seconds) {
    const float size = std::min(bounds.w, bounds.h) * 0.92f;
    const float cx = bounds.center_x();
    const float cy = bounds.center_y() + size * 0.015f;
    const float breathe = 1.0f + 0.012f * std::sin(time_seconds * 1.9f);

    const float body_rx = size * 0.275f;
    const float body_ry = size * 0.345f * breathe;

    const Color skin = visual.not_listening ? hex(0x3f4233) : hex(0x505238);
    const Color skin_shadow = visual.not_listening ? hex(0x303226) : hex(0x3f412b);
    const Color sclera = visual.not_listening ? hex(0x9d9d8d) : hex(0xe3dec7);
    const Color pupil = hex(0x16160f);
    const Color line = visual.not_listening ? hex(0x24261c) : hex(0x2a2c1e);

    // Soft ground shadow.
    canvas.fill_ellipse(cx, cy + body_ry * 1.03f, body_rx * 0.78f, size * 0.017f, hex(0x000000, 0.32f));

    // Legs and arms are intentionally simple shapes in M0. The real 2D
    // renderer will use the avatar image plus the face curve overlay.
    canvas.fill_ellipse(cx - body_rx * 0.38f, cy + body_ry * 1.02f, size * 0.044f, size * 0.155f, skin);
    canvas.fill_ellipse(cx + body_rx * 0.38f, cy + body_ry * 1.02f, size * 0.044f, size * 0.155f, skin);
    canvas.fill_ellipse(cx - body_rx * 1.00f, cy + body_ry * 0.55f, size * 0.032f, size * 0.180f, skin_shadow);
    canvas.fill_ellipse(cx + body_rx * 1.00f, cy + body_ry * 0.55f, size * 0.032f, size * 0.180f, skin_shadow);

    // Body.
    canvas.fill_ellipse(cx, cy, body_rx, body_ry, skin);
    canvas.fill_ellipse(cx - body_rx * 0.18f, cy - body_ry * 0.18f, body_rx * 0.72f, body_ry * 0.70f,
                        hex(0x6a6d4a, 0.07f));

    // Eyes.
    const float eye_dx = body_rx * 0.42f;
    const float eye_y = cy - body_ry * 0.30f;
    const float eye_r = size * 0.052f;
    canvas.fill_circle(cx - eye_dx, eye_y, eye_r, sclera);
    canvas.fill_circle(cx + eye_dx, eye_y, eye_r, sclera);

    const float gaze_y = visual.thinking ? -eye_r * 0.28f : 0.0f;
    canvas.fill_circle(cx - eye_dx, eye_y + gaze_y, eye_r * 0.42f, pupil);
    canvas.fill_circle(cx + eye_dx, eye_y + gaze_y, eye_r * 0.42f, pupil);

    if (visual.not_listening) {
        canvas.fill_ellipse(cx - eye_dx, eye_y - eye_r * 0.35f, eye_r * 1.15f, eye_r * 0.55f, skin);
        canvas.fill_ellipse(cx + eye_dx, eye_y - eye_r * 0.35f, eye_r * 1.15f, eye_r * 0.55f, skin);
    }

    // Nose.
    canvas.fill_triangle(cx, cy - body_ry * 0.10f,
                         cx - size * 0.018f, cy + body_ry * 0.16f,
                         cx + size * 0.018f, cy + body_ry * 0.16f, skin_shadow);

    // Mouth.
    if (visual.speaking) {
        const float open = 0.018f + 0.014f * std::abs(std::sin(time_seconds * 9.0f));
        canvas.fill_ellipse(cx, cy + body_ry * 0.33f, size * 0.038f, size * open, line);
    } else {
        canvas.stroke_line(cx - size * 0.085f, cy + body_ry * 0.33f,
                           cx + size * 0.085f, cy + body_ry * 0.33f, 2.2f, line);
    }
}

}  // namespace pk
