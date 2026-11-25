#include "app/car_ui.h"

#include "app/media_overlay.h"
#include "material-symbols/icons.h"
#include "puck/puck_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pk {
namespace {

constexpr float kRem = 16.0f;
// The car page uses env(safe-area-inset-*). The current display seam has no
// inset source, so kiosk builds use zero and keep the same formulas.
constexpr float kSafeTop = 0.0f;
constexpr float kSafeRight = 0.0f;
constexpr float kSafeBottom = 0.0f;
constexpr float kSafeLeft = 0.0f;

constexpr Color kBackground = hex(0x0b0d0b);
constexpr Color kForeground = hex(0xf6fff5);
constexpr Color kMuted = hex(0x9ca49c);
constexpr Color kSecondary = hex(0x2f372f);
constexpr Color kSuccess = hex(0x54b879);
constexpr Color kStart = hex(0x30d158);
constexpr Color kEnd = hex(0xff453a);
constexpr Color kDestructive = hex(0xd67670);
constexpr Color kWhite = hex(0xffffff);
constexpr Color kQrInk = hex(0x000000);

TextStyle type(float size, Color color, TextAlign align = TextAlign::Left,
               FontWeight weight = FontWeight::Regular, float tracking = 0.0f) {
    TextStyle style;
    style.size = size;
    style.color = color;
    style.align = align;
    style.weight = weight;
    style.tracking = tracking;
    return style;
}

Color fade(Color color, float alpha) {
    color.a *= alpha;
    return color;
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

bool compact_layout(Size size) {
    const bool portrait = size.height > size.width;
    return size.width <= 640 || (!portrait && size.height <= 500);
}

float hash01(std::uint32_t value) {
    value = (value << 13) ^ value;
    value = value * (value * value * 15731u + 789221u) + 1376312589u;
    return static_cast<float>(value & 0x7fffffffu) / static_cast<float>(0x7fffffffu);
}

float bezier_channel(float t, float p1, float p2) {
    const float u = 1.0f - t;
    return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
}

float cubic_bezier(float x1, float y1, float x2, float y2, float x) {
    if (x <= 0.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return 1.0f;
    }
    float low = 0.0f;
    float high = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float mid = (low + high) * 0.5f;
        if (bezier_channel(mid, x1, x2) < x) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return bezier_channel((low + high) * 0.5f, y1, y2);
}

std::vector<std::string> wrap_text(Canvas& canvas, const TextStyle& style, const std::string& text,
                                   float max_width) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t space = text.find(' ', index);
        const std::string word =
            text.substr(index, space == std::string::npos ? std::string::npos : space - index);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && max_width > 0.0f && canvas.measure_text(style, candidate) > max_width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        if (space == std::string::npos) {
            break;
        }
        index = space + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::string fit_text(Canvas& canvas, const TextStyle& style, const std::string& text,
                     float max_width) {
    if (max_width <= 0.0f || canvas.measure_text(style, text) <= max_width) {
        return text;
    }
    std::string result = text;
    while (!result.empty() && canvas.measure_text(style, result + "\u2026") > max_width) {
        result.pop_back();
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) {
            result.pop_back();
        }
    }
    return result.empty() ? std::string{} : result + "\u2026";
}

TextStyle fit_font_size(Canvas& canvas, TextStyle style, const std::string& text, float max_width,
                        float minimum_size) {
    while (style.size > minimum_size && canvas.measure_text(style, text) > max_width) {
        style.size -= 1.0f;
    }
    return style;
}

// Option labels use up to two lines. When the full label still cannot fit,
// the second line keeps the trailing words behind an ellipsis so options with
// the same long prefix remain distinguishable.
std::vector<std::string> fit_option_lines(Canvas& canvas, const TextStyle& style,
                                          const std::string& text, float max_width) {
    if (max_width <= 0.0f || canvas.measure_text(style, text) <= max_width) {
        return {text};
    }
    std::vector<std::string> wrapped = wrap_text(canvas, style, text, max_width);
    bool wrapped_fits = wrapped.size() <= 2;
    for (const std::string& line : wrapped) {
        if (canvas.measure_text(style, line) > max_width) {
            wrapped_fits = false;
        }
    }
    if (wrapped_fits && !wrapped.empty()) {
        return wrapped;
    }

    std::string first = wrapped.empty() ? std::string{} : wrapped.front();
    if (first.empty() || canvas.measure_text(style, first) > max_width) {
        first = fit_text(canvas, style, text, max_width);
    }
    std::size_t rest_start = first.size();
    while (rest_start < text.size() && text[rest_start] == ' ') {
        ++rest_start;
    }
    const std::string rest = text.substr(rest_start);
    std::vector<std::string> words;
    std::size_t word_start = 0;
    while (word_start < rest.size()) {
        const std::size_t space = rest.find(' ', word_start);
        words.push_back(rest.substr(word_start, space == std::string::npos
                                                   ? std::string::npos
                                                   : space - word_start));
        if (space == std::string::npos) {
            break;
        }
        word_start = space + 1;
    }
    std::string tail;
    for (std::size_t i = words.size(); i > 0; --i) {
        const std::string candidate =
            tail.empty() ? words[i - 1] : words[i - 1] + " " + tail;
        if (canvas.measure_text(style, "\u2026 " + candidate) <= max_width) {
            tail = candidate;
        } else {
            break;
        }
    }
    if (!tail.empty()) {
        return {first, "\u2026 " + tail};
    }
    if (!rest.empty()) {
        return {first, "\u2026 " + fit_text(canvas, style, rest, max_width - style.size)};
    }
    return {first};
}

void text_in_line(Canvas& canvas, float x, float top, float line_height, const TextStyle& style,
                  const std::string& value) {
    canvas.text(x, top + line_height * 0.5f, style, value);
}

void draw_option_label(Canvas& canvas, Rect button, const TextStyle& style,
                       const std::string& label) {
    const std::vector<std::string> lines =
        fit_option_lines(canvas, style, label, button.w - 24.0f);
    const float line_height = style.size * 1.14f;
    if (lines.size() <= 1 || button.h < line_height * 1.7f) {
        canvas.text(button.center_x(), button.center_y(), style,
                    fit_text(canvas, style, label, button.w - 24.0f));
        return;
    }
    const float top = button.center_y() - line_height;
    text_in_line(canvas, button.center_x(), top, line_height, style, lines[0]);
    text_in_line(canvas, button.center_x(), top + line_height, line_height, style, lines[1]);
}

float draw_wrapped_center(Canvas& canvas, float center_x, float top, const TextStyle& style,
                          float line_height, const std::string& text, float max_width) {
    const std::vector<std::string> lines = wrap_text(canvas, style, text, max_width);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text_in_line(canvas, center_x, top + static_cast<float>(i) * line_height, line_height,
                     style, lines[i]);
    }
    return static_cast<float>(lines.size()) * line_height;
}

void draw_round_button(Canvas& canvas, float center_x, float center_y, float diameter, Color fill,
                       float opacity) {
    canvas.fill_circle(center_x, center_y, diameter * 0.5f, fade(fill, opacity));
}

// Fills the lower part of a round button up to `level` as one polygon. The
// Canvas seam has no clipping path, so the chord outline keeps the fill inside
// the rim without a mask or per-slice seams.
void draw_round_level_fill(Canvas& canvas, float center_x, float center_y, float diameter,
                           float level, Color color, float opacity) {
    const float clamped = std::min(1.0f, std::max(0.0f, level));
    if (clamped <= 0.0f || diameter <= 0.0f) {
        return;
    }
    const float top = 100.0f * (1.0f - clamped);
    constexpr int kSegments = 28;
    std::string path = "M ";
    const auto point = [&](float x, float y, bool first) {
        if (!first) {
            path += " L ";
        }
        path += std::to_string(x);
        path += ',';
        path += std::to_string(y);
    };
    bool first = true;
    for (int i = 0; i <= kSegments; ++i) {
        const float y = top + (100.0f - top) * static_cast<float>(i) /
                                  static_cast<float>(kSegments);
        const float dy = y - 50.0f;
        const float half = std::sqrt(std::max(0.0f, 2500.0f - dy * dy));
        point(50.0f + half, y, first);
        first = false;
    }
    for (int i = kSegments; i >= 0; --i) {
        const float y = top + (100.0f - top) * static_cast<float>(i) /
                                  static_cast<float>(kSegments);
        const float dy = y - 50.0f;
        const float half = std::sqrt(std::max(0.0f, 2500.0f - dy * dy));
        point(50.0f - half, y, false);
    }
    path += " Z";
    const float radius = diameter * 0.5f;
    canvas.fill_path(path, Rect{0.0f, 0.0f, 100.0f, 100.0f},
                     Rect{center_x - radius, center_y - radius, diameter, diameter},
                     fade(color, opacity));
}

void draw_chevron(Canvas& canvas, float center_x, float center_y, float size, bool down,
                  Color color) {
    const float half = size * 0.5f;
    const float tip = center_y + (down ? half : -half);
    const float tail = center_y + (down ? -half : half);
    canvas.stroke_line(center_x - half, tail, center_x, tip, 3.0f, color);
    canvas.stroke_line(center_x, tip, center_x + half, tail, 3.0f, color);
}

constexpr Rect kIconBox{0.0f, -960.0f, 960.0f, 960.0f};

Rect icon_rect(float center_x, float center_y, float size) {
    return Rect{center_x - size * 0.5f, center_y - size * 0.5f, size, size};
}

void draw_call_icon(Canvas& canvas, float center_x, float center_y, float size, Color color) {
    canvas.fill_path(icons::kCall, kIconBox, icon_rect(center_x, center_y, size), color);
}

void draw_end_icon(Canvas& canvas, float center_x, float center_y, float size, Color color) {
    canvas.fill_path(icons::kCallEnd, kIconBox, icon_rect(center_x, center_y, size), color);
}

void draw_mute_icon(Canvas& canvas, float center_x, float center_y, float size, Color color,
                    bool muted) {
    canvas.fill_path(muted ? icons::kMicOff : icons::kMic, kIconBox,
                     icon_rect(center_x, center_y, size), color);
}

void draw_lock_icon(Canvas& canvas, float center_x, float center_y, float size, Color color) {
    canvas.fill_path(icons::kLock, kIconBox, icon_rect(center_x, center_y, size), color);
}

void draw_qr_card(Canvas& canvas, Rect card, float radius, const QrCode& qr) {
    canvas.fill_rounded_rect(card, radius, kWhite);
    if (qr.empty()) {
        const float box = card.w * 0.32f;
        const Rect inner{card.center_x() - box * 0.5f, card.center_y() - box * 0.5f, box, box};
        canvas.fill_rounded_rect(inner, box * 0.12f, hex(0x0b0d0b, 0.08f));
        canvas.text(inner.center_x(), inner.center_y(), type(box * 0.22f, kMuted, TextAlign::Center),
                    "QR");
        return;
    }

    const int total_modules = qr.size + 8;
    const float inner = card.w - 12.0f;
    const float cell = inner / static_cast<float>(total_modules);
    const float origin_x = card.center_x() - inner * 0.5f;
    const float origin_y = card.center_y() - inner * 0.5f;
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.dark(x, y)) {
                continue;
            }
            canvas.fill_rect(Rect{origin_x + static_cast<float>(x + 4) * cell,
                                  origin_y + static_cast<float>(y + 4) * cell, cell + 0.35f,
                                  cell + 0.35f},
                             kQrInk);
        }
    }

    // The web puts the Amp mark in the middle. The native pairing encoder uses
    // EC-M, so keep the mark well below the web's 35% to preserve the code.
    const float logo_size = card.w * 0.14f;
    const Rect logo{card.center_x() - logo_size * 0.5f, card.center_y() - logo_size * 0.5f,
                    logo_size, logo_size};
    canvas.fill_rounded_rect(logo, logo_size * 0.22f, kQrInk);
    const TextStyle logo_style = type(logo_size * 0.32f, kWhite, TextAlign::Center);
    canvas.text(logo.center_x(), logo.center_y(), logo_style,
                fit_text(canvas, logo_style, "amp", logo_size * 0.88f));
}

void draw_call_backdrop(Canvas& canvas, Rect stage, Rect puck, float time_seconds, float light) {
    if (light <= 0.002f) {
        return;
    }
    for (int i = 0; i < 56; ++i) {
        const float x = stage.x + stage.w * (0.06f + 0.88f * hash01(static_cast<std::uint32_t>(i * 4 + 1)));
        const float y = stage.y + stage.h * (0.06f + 0.88f * hash01(static_cast<std::uint32_t>(i * 4 + 2)));
        const float phase = hash01(static_cast<std::uint32_t>(i * 4 + 3));
        const float twinkle = 0.5f + 0.5f * std::sin(time_seconds * 1.1f + phase * 6.2831853f);
        const float alpha = (0.025f + 0.075f * twinkle) * light;
        const float radius = 0.6f + 1.0f * hash01(static_cast<std::uint32_t>(i * 4 + 4));
        canvas.fill_circle(x, y, radius, hex(0xdfe8d8, alpha));
    }

    const float foot_y = puck.y + puck.h * 0.93f;
    for (int i = 10; i >= 1; --i) {
        const float t = static_cast<float>(i) / 10.0f;
        canvas.fill_ellipse(puck.center_x(), foot_y, puck.w * (0.10f + 0.30f * t),
                            puck.h * (0.010f + 0.045f * t), hex(0xb8c7a2, 0.010f * light));
    }
    const float top_y = puck.y + puck.h * 0.06f;
    for (int i = 12; i >= 1; --i) {
        const float t = static_cast<float>(i) / 12.0f;
        canvas.fill_triangle(puck.center_x() - puck.w * (0.03f + 0.10f * t), top_y,
                             puck.center_x() + puck.w * (0.03f + 0.10f * t), top_y,
                             puck.center_x() + puck.w * (0.10f + 0.34f * t), foot_y,
                             hex(0xb8c7a2, 0.004f * light));
    }
}

struct PairingLayout {
    bool portrait = false;
    bool compact = false;
    Rect stage;
    Rect side;
    Rect title;
    Rect slot;
    Rect card;
    Rect status;
    Rect retry;
    Rect debug;
    TextStyle title_style;
    TextStyle status_style;
    TextStyle retry_style;
    TextStyle code_style;
    float title_line = 60.0f;
    float side_gap = 24.0f;
    float card_radius = 20.0f;
    float retry_height = 0.0f;
    bool show_status = false;
    bool show_retry = false;
    bool show_card = true;
};

PairingLayout pairing_layout(Canvas& canvas, Size size, const std::string& title,
                             const std::string& code, const std::string& status,
                             bool show_retry, bool show_card) {
    PairingLayout layout;
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    layout.portrait = height > width;
    const bool short_landscape = !layout.portrait && height <= 500.0f;
    layout.compact = width <= 640.0f || short_landscape;

    float qr = 0.0f;
    if (layout.portrait) {
        qr = width <= 640.0f ? std::min(height * 0.22f, width - 128.0f)
                             : std::min({height * 0.22f, width * 0.60f, 256.0f});
    } else if (short_landscape) {
        qr = std::min(height * 0.50f, width * 0.30f);
    } else {
        qr = std::min(height * 0.36f, 288.0f);
    }
    qr = std::max(80.0f, qr);
    float card = qr + 12.0f;
    layout.card_radius = layout.compact ? 16.0f : 20.0f;

    const float title_size =
        layout.compact ? (layout.portrait ? 36.0f : (short_landscape ? 24.0f : 28.0f)) : 48.0f;
    layout.title_line =
        layout.compact ? (layout.portrait ? 40.0f : (short_landscape ? 28.0f : 32.0f)) : 60.0f;
    layout.title_style =
        type(title_size, kForeground, TextAlign::Center, FontWeight::Semibold, -0.025f);
    const float retry_size = layout.compact ? 18.0f : 24.0f;
    const float retry_pad = layout.compact ? 24.0f : 32.0f;
    layout.retry_height = layout.compact ? 44.0f : 56.0f;
    layout.retry_style =
        type(retry_size, kForeground, TextAlign::Center, FontWeight::Medium);
    const float status_size = layout.compact ? 16.0f : 20.0f;
    const float status_height = layout.compact ? 22.0f : 28.0f;
    layout.status_style = type(status_size, kMuted, TextAlign::Center);
    const float code_size = layout.compact ? 56.0f : 72.0f;
    layout.code_style =
        type(code_size, kForeground, TextAlign::Center, FontWeight::Medium, 0.2f);
    layout.side_gap = layout.portrait ? (width <= 640.0f ? 16.0f : 24.0f)
                                      : (short_landscape ? 12.0f : 24.0f);

    float pad_top = 0.0f;
    float pad_right = 0.0f;
    float pad_bottom = 0.0f;
    float pad_left = 0.0f;
    if (layout.portrait) {
        pad_top = 16.0f;
        pad_bottom = width <= 640.0f ? 64.0f : 80.0f;
        pad_left = pad_right = 24.0f;
    } else {
        pad_left = 32.0f;
        pad_right = short_landscape ? 40.0f : 80.0f;
    }
    pad_top += kSafeTop;
    pad_right += kSafeRight;
    pad_bottom += kSafeBottom;
    pad_left += kSafeLeft;
    const float gap = layout.portrait ? 24.0f : (short_landscape ? 24.0f : 48.0f);
    layout.show_status = code.empty() && !status.empty();
    layout.show_retry = show_retry;
    layout.show_card = show_card;
    layout.retry_height = layout.show_retry ? layout.retry_height : 0.0f;

    float side_width = card;
    if (layout.portrait) {
        const float available = width - pad_left - pad_right;
        const float title_width = canvas.measure_text(layout.title_style, title);
        const float retry_width =
            canvas.measure_text(layout.retry_style, "Try Again") + retry_pad * 2.0f;
        side_width = std::min(available, std::max({card, title_width, retry_width}));
        side_width = std::max(side_width, std::min(card, available));
    } else if (short_landscape && width < 700.0f) {
        const float max_side =
            std::max(card, width - pad_left - pad_right - gap - 80.0f);
        side_width = std::min(max_side, std::max(card, width * 0.42f));
    }

    const std::vector<std::string> title_lines =
        wrap_text(canvas, layout.title_style, title, side_width);
    const float title_height =
        static_cast<float>(std::max<std::size_t>(1, title_lines.size())) * layout.title_line;

    // A short landscape window cannot stack a QR card, title, status, and
    // recovery button at the default card size. Give the recovery block its
    // height first and shrink the card to the remainder.
    if (short_landscape && layout.show_card) {
        const float fixed = title_height + layout.side_gap +
                            (layout.show_status ? layout.side_gap + status_height : 0.0f) +
                            (layout.show_retry ? layout.side_gap + layout.retry_height : 0.0f);
        const float available_height = height - pad_top - pad_bottom - 8.0f;
        const float max_card = std::max(60.0f, available_height - fixed);
        card = std::min(card, max_card);
    }

    const float side_height = title_height +
                              (layout.show_card ? layout.side_gap + card : 0.0f) +
                              (layout.show_status ? layout.side_gap + status_height : 0.0f) +
                              (layout.show_retry ? layout.side_gap + layout.retry_height : 0.0f);

    if (layout.portrait) {
        const float stage_height =
            std::max(0.0f, height - pad_top - pad_bottom - gap - side_height);
        layout.stage = Rect{pad_left, pad_top, width - pad_left - pad_right, stage_height};
        layout.side = Rect{(width - side_width) * 0.5f, pad_top + stage_height + gap, side_width,
                           side_height};
    } else {
        const float stage_width =
            std::max(0.0f, width - pad_left - pad_right - gap - side_width);
        layout.stage = Rect{pad_left, pad_top, stage_width, height - pad_top - pad_bottom};
        layout.side = Rect{width - pad_right - side_width,
                           pad_top + (height - pad_top - pad_bottom - side_height) * 0.5f,
                           side_width, side_height};
    }

    layout.title = Rect{layout.side.x, layout.side.y, layout.side.w, title_height};
    float content_bottom = layout.title.y + layout.title.h;
    if (layout.show_card) {
        layout.slot = Rect{layout.side.x, content_bottom + layout.side_gap, layout.side.w, card};
        layout.card = Rect{layout.slot.x, layout.slot.y, card, card};
        content_bottom = layout.slot.y + layout.slot.h;
    }
    if (layout.show_status) {
        layout.status = Rect{layout.side.x, content_bottom + layout.side_gap, layout.side.w,
                             status_height};
        content_bottom = layout.status.y + layout.status.h;
    }
    if (layout.show_retry) {
        const float retry_width =
            std::min(layout.side.w,
                     canvas.measure_text(layout.retry_style, "Try Again") + retry_pad * 2.0f);
        layout.retry = Rect{layout.side.center_x() - retry_width * 0.5f,
                            content_bottom + layout.side_gap, retry_width,
                            layout.retry_height};
    }

    const float debug_height = layout.compact ? 44.0f : 64.0f;
    const float debug_font = layout.compact ? 16.0f : 20.0f;
    const float debug_pad = layout.compact ? 16.0f : 24.0f;
    const float debug_width =
        canvas.measure_text(type(debug_font, kForeground), "Debug") + debug_pad * 2.0f;
    const float margin = layout.compact ? 12.0f : 40.0f;
    layout.debug = Rect{margin + kSafeLeft, height - margin - debug_height - kSafeBottom,
                        debug_width, debug_height};
    return layout;
}

struct CallLayout {
    bool wide = false;
    bool compact = false;
    bool portrait = false;
    Rect header;
    Rect main;
    Rect main_content;
    Rect puck_space;
    Rect puck;
    Rect caption;
    Rect controls;
    Rect overlay;
    float header_height = 96.0f;
    float header_pad = 40.0f;
    float button_size = 136.0f;
    float split_x = 100.0f;
    float split_y = 0.0f;
    float icon_size = 52.0f;
    float caption_size = 40.0f;
    float caption_height = 48.0f;
    float controls_pad_x = 0.0f;
    float controls_pad_bottom = 24.0f;
    float control_center_x = 0.0f;
    float control_center_y = 0.0f;
};

CallLayout call_layout(Size size) {
    CallLayout layout;
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    layout.portrait = height > width;
    const bool short_landscape = !layout.portrait && height <= 500.0f;
    layout.compact = width <= 640.0f || short_landscape;
    layout.wide = !layout.portrait && width >= 900.0f && height >= 600.0f;
    layout.header_height = layout.compact ? 56.0f : 96.0f;
    layout.header_pad = layout.compact ? 12.0f : 40.0f;

    if (layout.wide) {
        const float rail = std::clamp(width * 0.26f, 288.0f, 384.0f);
        layout.header = Rect{0.0f, 0.0f, rail, layout.header_height};
        layout.main = Rect{rail, 0.0f, width - rail, height};
        layout.controls = Rect{0.0f, layout.header_height, rail, height - layout.header_height};
        layout.controls_pad_x = 32.0f;
        layout.controls_pad_bottom = 64.0f;
        layout.button_size = 176.0f;
        layout.split_x = 0.0f;
        layout.split_y = -112.0f;
        layout.icon_size = 64.0f;
        layout.caption_size = 40.0f;
        layout.caption_height = 48.0f;
        layout.main_content =
            Rect{layout.main.x + 32.0f, 64.0f + kSafeTop, layout.main.w - 64.0f,
                 height - 128.0f - kSafeTop - kSafeBottom};
        layout.overlay = Rect{rail, kSafeTop, width - rail - kSafeRight, height - 88.0f - kSafeBottom};
    } else if (short_landscape) {
        // Keep the control rail usable but do not let it swallow a narrow
        // window: the plugin sheet needs width for its option and scroll rail.
        const float controls_width = std::clamp(width * 0.36f, 120.0f, 272.0f);
        layout.header = Rect{0.0f, 0.0f, width, layout.header_height};
        layout.main = Rect{0.0f, layout.header_height, width - controls_width,
                           height - layout.header_height};
        layout.controls = Rect{width - controls_width, layout.header_height, controls_width,
                               height - layout.header_height};
        layout.controls_pad_x = 16.0f;
        layout.controls_pad_bottom = 16.0f;
        layout.button_size = 88.0f;
        layout.split_x = 0.0f;
        layout.split_y = -56.0f;
        layout.icon_size = 36.0f;
        layout.caption_size = 20.0f;
        layout.caption_height = 32.0f;
        layout.main_content = Rect{layout.main.x + kSafeLeft, layout.main.y + 40.0f + kSafeTop,
                                   layout.main.w - kSafeLeft - kSafeRight,
                                   layout.main.h - 80.0f - kSafeTop - kSafeBottom};
        layout.overlay = Rect{0.0f, layout.header_height + kSafeTop,
                              width - controls_width - kSafeRight,
                              height - layout.header_height - kSafeTop - kSafeBottom};
    } else {
        const float controls_height = layout.compact ? 160.0f : 208.0f;
        layout.header = Rect{0.0f, 0.0f, width, layout.header_height};
        layout.controls = Rect{0.0f, height - controls_height - kSafeBottom, width,
                               controls_height};
        layout.main = Rect{0.0f, layout.header_height, width,
                           height - layout.header_height - controls_height - kSafeBottom};
        layout.controls_pad_x = 0.0f;
        layout.controls_pad_bottom = layout.compact ? 16.0f : 24.0f;
        layout.button_size = layout.compact ? 88.0f : 136.0f;
        layout.split_x = layout.compact ? 68.0f : 100.0f;
        layout.split_y = 0.0f;
        layout.icon_size = layout.compact ? 36.0f : 52.0f;
        layout.caption_size = layout.compact ? 24.0f : 40.0f;
        layout.caption_height = layout.compact ? 36.0f : 48.0f;
        const float pad_y = layout.compact ? 48.0f : 64.0f;
        layout.main_content = Rect{layout.main.x, layout.main.y + pad_y + kSafeTop,
                                   layout.main.w, layout.main.h - pad_y * 2.0f - kSafeTop};
        layout.overlay = Rect{0.0f, kSafeTop, width, height - controls_height - kSafeBottom};
    }

    layout.puck_space = layout.main_content;
    const float puck_size =
        std::max(0.0f, std::min(layout.puck_space.w - 32.0f, layout.puck_space.h));
    layout.puck = Rect{layout.puck_space.center_x() - puck_size * 0.5f,
                       layout.puck_space.center_y() - puck_size * 0.5f, puck_size, puck_size};

    const float caption_pad = layout.compact ? 16.0f : 32.0f;
    layout.caption = Rect{layout.main.x + caption_pad,
                          layout.main.y + layout.main.h - 8.0f - layout.caption_height,
                          layout.main.w - caption_pad * 2.0f, layout.caption_height};

    if (layout.wide || short_landscape) {
        layout.control_center_x = layout.controls.center_x();
        layout.control_center_y =
            layout.controls.y + (layout.controls.h - layout.controls_pad_bottom) * 0.5f;
    } else {
        layout.control_center_x = width * 0.5f;
        layout.control_center_y =
            layout.controls.y + (layout.controls.h - layout.controls_pad_bottom) * 0.5f;
    }
    return layout;
}

std::string call_caption(const CallView& call) {
    if (call.closing) {
        return "Ending call";
    }
    if (call.dictation_paused && !call.muted) {
        return "Microphone paused";
    }
    switch (call.phase) {
        case CallPhase::Starting:
        case CallPhase::Connecting:
            return "Connecting";
        case CallPhase::Thinking:
            return "Thinking";
        case CallPhase::Ended:
            return "Call Ended";
        case CallPhase::Failed:
            return call.detail.empty() ? "Call failed" : call.detail;
        case CallPhase::Muted:
            return call.caption.empty() ? "Microphone off" : call.caption;
        case CallPhase::Idle:
        case CallPhase::Active:
        case CallPhase::Speaking:
            // The normal listening and speaking states stay wordless: only
            // connection, pause, ended, and failure get a state line.
            return {};
    }
    return {};
}

void draw_call_header(Canvas& canvas, const CallLayout& layout, Size size) {
    const float logo_size = layout.compact ? 20.0f : 28.0f;
    const TextAlign align = layout.compact ? TextAlign::Center : TextAlign::Left;
    const float x = layout.compact ? static_cast<float>(size.width) * 0.5f
                                   : layout.header.x + layout.header_pad;
    canvas.text(x, layout.header.center_y(), type(logo_size, kMuted, align), "amp");
}

void draw_call_caption(Canvas& canvas, const CallLayout& layout, const CallView& call) {
    const std::string text = call_caption(call);
    if (text.empty()) {
        return;
    }
    const Color color = call.phase == CallPhase::Failed ? kDestructive : kMuted;
    const TextStyle style = type(layout.caption_size, color, TextAlign::Center, FontWeight::Medium);
    canvas.text(layout.caption.center_x(), layout.caption.center_y(), style,
                fit_text(canvas, style, text, layout.caption.w));
}

Rect confirm_rect(const CallLayout& layout) {
    if (layout.wide) {
        const float width = layout.controls.w - layout.controls_pad_x * 2.0f;
        const float height = 128.0f;
        return Rect{layout.controls.x + layout.controls_pad_x,
                    layout.controls.y +
                        (layout.controls.h - layout.controls_pad_bottom - height) * 0.5f,
                    width, height};
    }
    if (layout.compact && !layout.portrait) {
        const float width = std::min(layout.controls.w - layout.controls_pad_x * 2.0f - 32.0f,
                                     26.0f * kRem);
        return Rect{layout.controls.center_x() - width * 0.5f, layout.control_center_y - 40.0f,
                    width, 80.0f};
    }
    if (layout.compact) {
        const float width = std::min(static_cast<float>(layout.main.w) - 32.0f, 26.0f * kRem);
        return Rect{layout.control_center_x - width * 0.5f, layout.control_center_y - 40.0f,
                    width, 80.0f};
    }
    const float width = std::min(layout.controls.w - 26.0f * kRem, 32.0f * kRem);
    return Rect{layout.control_center_x - width * 0.5f, layout.control_center_y - 64.0f, width,
                128.0f};
}

void draw_confirm_button(Canvas& canvas, const CallLayout& layout, const PasskeyView& passkey,
                         UiHitMap& hits) {
    const Rect rect = confirm_rect(layout);
    const bool confirming = passkey.confirming;
    canvas.fill_rounded_rect(rect, rect.h * 0.5f, confirming ? kSecondary : kSuccess);

    const float font = layout.compact ? 26.0f : (layout.wide ? 32.0f : 36.0f);
    TextStyle label_style = type(font, confirming ? kMuted : kWhite, TextAlign::Center,
                                 FontWeight::Semibold);
    const std::string label = confirming ? "Confirming\u2026" : "Confirm Passkey";
    const float icon_size = layout.compact ? 32.0f : 44.0f;
    const float gap = layout.compact ? 12.0f : 20.0f;
    const float inset = 16.0f;
    const float available = rect.w - inset * 2.0f;
    const float label_width = canvas.measure_text(label_style, label);
    const bool with_icon = !confirming && label_width + icon_size + gap <= available;
    if (with_icon) {
        const float group = icon_size + gap + label_width;
        const float start = rect.center_x() - group * 0.5f;
        draw_lock_icon(canvas, start + icon_size * 0.5f, rect.center_y(), icon_size, kWhite);
        canvas.text(start + icon_size + gap, rect.center_y(), label_style, label);
    } else {
        label_style = fit_font_size(canvas, label_style, label, available, 20.0f);
        canvas.text(rect.center_x(), rect.center_y(), label_style,
                    fit_text(canvas, label_style, label, available));
    }
    if (!confirming) {
        hits.add(rect, UiAction::PasskeyConfirm);
    }
}

void draw_call_controls(Canvas& canvas, const CallLayout& layout, const CallView& call,
                        const PasskeyView& passkey, float split, float mic_level,
                        bool call_available, UiHitMap& hits) {
    // The web keeps the call object (and its End/Mute controls) after a failed
    // call; only ending it returns to the green call button.
    const bool in_call = call.in_call() || call.phase == CallPhase::Failed;
    if ((passkey.confirm_button || passkey.confirming) && !in_call) {
        draw_confirm_button(canvas, layout, passkey, hits);
        return;
    }

    if (!in_call) {
        const float center_x = layout.control_center_x + layout.split_x * split;
        const float center_y = layout.control_center_y + layout.split_y * split;
        const bool can_call = call_available && passkey.mode != PasskeyMode::Phone;
        const float opacity = can_call ? 1.0f : 0.4f;
        draw_round_button(canvas, center_x, center_y, layout.button_size, kStart, opacity);
        draw_call_icon(canvas, center_x, center_y, layout.icon_size, fade(kWhite, opacity));
        if (can_call) {
            const float radius = layout.button_size * 0.5f;
            hits.add(Rect{center_x - radius, center_y - radius, layout.button_size,
                          layout.button_size},
                     UiAction::Call);
        }
        return;
    }

    const float end_x = layout.control_center_x + layout.split_x * split;
    const float end_y = layout.control_center_y + layout.split_y * split;
    const float mute_x = layout.control_center_x - layout.split_x * split;
    const float mute_y = layout.control_center_y - layout.split_y * split;
    const float radius = layout.button_size * 0.5f;
    const bool disabled = call.phase == CallPhase::Starting ||
                          call.phase == CallPhase::Connecting || call.phase == CallPhase::Failed;

    draw_round_button(canvas, end_x, end_y, layout.button_size, kEnd, 1.0f);
    draw_end_icon(canvas, end_x, end_y, layout.icon_size, kWhite);
    hits.add(Rect{end_x - radius, end_y - radius, layout.button_size, layout.button_size},
             UiAction::End);

    const float presence = clamp01(split * 2.0f);
    const float opacity = presence * (disabled ? 0.4f : 1.0f);
    const float scale = 0.65f + 0.35f * presence;
    const Color fill = call.muted ? kForeground : kSecondary;
    const Color icon = call.muted ? kBackground : kForeground;
    draw_round_button(canvas, mute_x, mute_y, layout.button_size * scale, fill, opacity);
    if (!call.muted && !disabled && mic_level > 0.0f) {
        draw_round_level_fill(canvas, mute_x, mute_y, layout.button_size * scale, mic_level,
                              kSuccess, opacity * 0.55f);
    }
    draw_mute_icon(canvas, mute_x, mute_y, layout.icon_size * scale, fade(icon, opacity),
                   call.muted);
    if (!disabled) {
        hits.add(Rect{mute_x - radius, mute_y - radius, layout.button_size, layout.button_size},
                 UiAction::Mute);
    }
}

enum class HintBadge {
    A,
    B,
    DPad,
};

struct HintItem {
    HintBadge badge = HintBadge::A;
    std::string label;
};

std::string focus_hint_label(UiAction action, bool holding, bool muted) {
    switch (action) {
        case UiAction::Call: return "Call";
        case UiAction::Mute: return muted ? "Unmute" : "Mute";
        case UiAction::End: return holding ? "Keep holding" : "Hold to end call";
        case UiAction::MediaClose: return "Close";
        case UiAction::MediaPrevious: return "Previous";
        case UiAction::MediaNext: return "Next";
        case UiAction::SheetOption: return "Select";
        case UiAction::SheetDismiss: return "Dismiss";
        case UiAction::SheetOk: return "OK";
        case UiAction::SheetScrollUp: return "Scroll up";
        case UiAction::SheetScrollDown: return "Scroll down";
        case UiAction::PairingRetry: return "Try again";
        case UiAction::PasskeyConfirm: return "Confirm";
        case UiAction::DebugOpen: return "Open";
        case UiAction::DebugClose: return "Close";
        case UiAction::EchoMode: return "Select";
        case UiAction::MicTest: return "Test mic";
        case UiAction::SendDiagnostics: return "Send";
        case UiAction::None:
        case UiAction::WindowMinimize:
        case UiAction::WindowMaximize:
        case UiAction::WindowClose:
        case UiAction::Escape:
            return {};
    }
    return {};
}

void draw_hint_badge(Canvas& canvas, HintBadge badge, float center_x, float center_y,
                     float diameter, Color fill, Color ink) {
    canvas.fill_circle(center_x, center_y, diameter * 0.5f, fill);
    if (badge == HintBadge::A || badge == HintBadge::B) {
        const TextStyle style =
            type(diameter * 0.62f, ink, TextAlign::Center, FontWeight::Semibold);
        canvas.text(center_x, center_y, style, badge == HintBadge::A ? "A" : "B");
        return;
    }
    if (badge == HintBadge::DPad) {
        canvas.fill_rect(Rect{center_x - diameter * 0.28f, center_y - diameter * 0.09f,
                              diameter * 0.56f, diameter * 0.18f},
                         ink);
        canvas.fill_rect(Rect{center_x - diameter * 0.09f, center_y - diameter * 0.28f,
                              diameter * 0.18f, diameter * 0.56f},
                         ink);
    }
}

void draw_hold_ring(Canvas& canvas, Rect rect, float progress) {
    const float radius = std::min(rect.w, rect.h) * 0.5f + 8.0f;
    const float center_x = rect.center_x();
    const float center_y = rect.center_y();
    canvas.stroke_circle(center_x, center_y, radius, 4.0f, hex(0x54b879, 0.22f));
    const int segments = 64;
    const int filled = static_cast<int>(std::ceil(clamp01(progress) * segments));
    const float step = 6.2831853f / static_cast<float>(segments);
    for (int i = 0; i < filled; ++i) {
        const float start = -1.5707963f + step * static_cast<float>(i);
        const float end = start + step * 0.82f;
        canvas.stroke_line(center_x + std::cos(start) * radius,
                           center_y + std::sin(start) * radius,
                           center_x + std::cos(end) * radius,
                           center_y + std::sin(end) * radius, 4.0f, kSuccess);
    }
}

}  // namespace

void CarUi::add_target(Rect rect, UiAction action, int index) {
    hits_.add(rect, action, index);
}

void CarUi::notify_input(const InputEvent& event) {
    if (is_gamepad_event(event)) {
        hints_visible_ = true;
        if (event.type == InputEvent::Type::GamepadButtonDown) {
            if (event.button == InputEvent::GamepadButton::A && focus_.focused) {
                if (focus_.action == UiAction::SheetScrollUp) {
                    scroll_sheet(-1);
                    return;
                }
                if (focus_.action == UiAction::SheetScrollDown) {
                    scroll_sheet(1);
                    return;
                }
            }
            if (sheet_scroll_max_ > 0) {
                if (event.button == InputEvent::GamepadButton::LeftBumper) {
                    scroll_sheet(-std::max(1, sheet_scroll_capacity_));
                } else if (event.button == InputEvent::GamepadButton::RightBumper) {
                    scroll_sheet(std::max(1, sheet_scroll_capacity_));
                }
            }
        }
        return;
    }
    if (event.type == InputEvent::Type::KeyDown && !event.text) {
        if (sheet_scroll_max_ > 0) {
            // X11 and evdev key codes are both accepted, like Escape above.
            const int page = std::max(1, sheet_scroll_capacity_);
            switch (event.key) {
                case 38:   // X11 Up
                case 103:  // evdev KEY_UP
                    if (scroll_sheet(-1)) {
                        return;
                    }
                    break;
                case 40:   // X11 Down
                case 108:  // evdev KEY_DOWN
                    if (scroll_sheet(1)) {
                        return;
                    }
                    break;
                case 33:   // X11 PageUp
                case 104:  // evdev KEY_PAGEUP
                    if (scroll_sheet(-page)) {
                        return;
                    }
                    break;
                case 34:   // X11 PageDown
                case 109:  // evdev KEY_PAGEDOWN
                    if (scroll_sheet(page)) {
                        return;
                    }
                    break;
                case 36:   // X11 Home
                case 102:  // evdev KEY_HOME
                    if (scroll_sheet(-sheet_scroll_row_)) {
                        return;
                    }
                    break;
                case 35:   // X11 End
                case 107:  // evdev KEY_END
                    if (scroll_sheet(sheet_scroll_max_ - sheet_scroll_row_)) {
                        return;
                    }
                    break;
                default:
                    break;
            }
        }
        if (event.key == 123 || event.key == 88) {
            debug_revealed_ = !debug_revealed_;
            return;
        }
    }
    if (event.type == InputEvent::Type::PointerDown ||
        event.type == InputEvent::Type::PointerMove ||
        event.type == InputEvent::Type::PointerUp) {
        hints_visible_ = false;
        if (event.type == InputEvent::Type::PointerDown && sheet_scroll_max_ > 0) {
            if (rect_contains(sheet_scroll_up_, event.x, event.y) && scroll_sheet(-1)) {
                return;
            }
            if (rect_contains(sheet_scroll_down_, event.x, event.y) && scroll_sheet(1)) {
                return;
            }
        }
        const float corner = 96.0f;
        const bool in_corner = event.x < corner &&
                               event.y > static_cast<float>(last_size_.height) - corner;
        if (event.type == InputEvent::Type::PointerDown && in_corner) {
            corner_holding_ = true;
            corner_hold_start_ = last_draw_time_;
            if (tap_window_start_ < 0.0f || last_draw_time_ - tap_window_start_ > 2.5f) {
                corner_taps_ = 0;
                tap_window_start_ = last_draw_time_;
            }
            ++corner_taps_;
            if (corner_taps_ >= 5) {
                debug_revealed_ = true;
                corner_taps_ = 0;
                tap_window_start_ = -1.0f;
            }
        } else if (!in_corner || event.type == InputEvent::Type::PointerUp) {
            corner_holding_ = false;
        }
    }
}

void CarUi::draw_focus_overlay(Canvas& canvas, Size size) {
    if (!focus_.visible && !focus_.holding) {
        return;
    }

    const UiTarget* target = nullptr;
    if (focus_.focused) {
        for (const UiTarget& candidate : hits_.targets()) {
            if (candidate.action == focus_.action && candidate.index == focus_.index) {
                target = &candidate;
                break;
            }
        }
    }

    if (target) {
        const Rect rect = target->rect;
        const bool round = target->action == UiAction::Call ||
                           target->action == UiAction::Mute ||
                           target->action == UiAction::End ||
                           target->action == UiAction::MediaPrevious ||
                           target->action == UiAction::MediaNext;
        if (round) {
            canvas.stroke_circle(rect.center_x(), rect.center_y(),
                                 std::min(rect.w, rect.h) * 0.5f + 4.0f, 3.0f, kSuccess);
        } else {
            const Rect outline{rect.x - 3.0f, rect.y - 3.0f, rect.w + 6.0f, rect.h + 6.0f};
            canvas.stroke_line(outline.x, outline.y, outline.x + outline.w, outline.y, 3.0f,
                               kSuccess);
            canvas.stroke_line(outline.x, outline.y + outline.h, outline.x + outline.w,
                               outline.y + outline.h, 3.0f, kSuccess);
            canvas.stroke_line(outline.x, outline.y, outline.x, outline.y + outline.h, 3.0f,
                               kSuccess);
            canvas.stroke_line(outline.x + outline.w, outline.y, outline.x + outline.w,
                               outline.y + outline.h, 3.0f, kSuccess);
        }
        if (focus_.holding && target->action == UiAction::End) {
            draw_hold_ring(canvas, rect, focus_.hold_progress);
        }
    }

    // Pointer and touch input hide the gamepad hint bar, but a hold in progress
    // still draws its ring above.
    if (!focus_.visible) {
        return;
    }

    const bool compact = compact_layout(size);
    const float font = compact ? 14.0f : 16.0f;
    const TextStyle label_style =
        type(font, kForeground, TextAlign::Left, FontWeight::Medium);
    const float badge = compact ? 20.0f : 24.0f;
    const float label_gap = 7.0f;
    const float item_gap = compact ? 14.0f : 22.0f;

    std::vector<HintItem> items;
    if (focus_.focused) {
        const std::string label = focus_hint_label(focus_.action, focus_.holding, call_.muted);
        if (!label.empty()) {
            items.push_back(HintItem{HintBadge::A, label});
        }
    }
    items.push_back(HintItem{HintBadge::DPad, "Navigate"});
    const bool overlay_open = debug_.open || sheet_.kind != PluginSheetKind::None ||
                              (media_ && media_->visible());
    if (overlay_open) {
        items.push_back(HintItem{HintBadge::B, "Back"});
    }

    auto total_width = [&]() {
        float total = 0.0f;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                total += item_gap;
            }
            total += badge + label_gap + canvas.measure_text(label_style, items[i].label);
        }
        return total;
    };
    const float available = static_cast<float>(size.width) - 24.0f;
    auto drop = [&](HintBadge wanted) {
        for (std::size_t i = 1; i < items.size(); ++i) {
            if (items[i].badge == wanted) {
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    };
    while (total_width() > available && items.size() > 1) {
        if (drop(HintBadge::DPad) || drop(HintBadge::B)) {
            continue;
        }
        break;
    }

    const float bar_height = badge + 8.0f;
    const float bar_width = total_width() + 20.0f;
    // Corner placement: bottom-right first, bottom-left if it would cover a
    // control; if both collide, sit above the controls instead of over them.
    const float margin = 12.0f;
    const auto collides = [&](const Rect& bar) {
        for (const UiTarget& target : hits_.targets()) {
            const Rect& r = target.rect;
            if (bar.x < r.x + r.w && r.x < bar.x + bar.w && bar.y < r.y + r.h &&
                r.y < bar.y + bar.h) {
                return true;
            }
        }
        return false;
    };
    Rect bar{static_cast<float>(size.width) - bar_width - margin,
             static_cast<float>(size.height) - bar_height - margin, bar_width, bar_height};
    if (collides(bar)) {
        const Rect left{margin, static_cast<float>(size.height) - bar_height - margin, bar_width,
                        bar_height};
        if (!collides(left)) {
            bar = left;
        } else {
            float top = static_cast<float>(size.height);
            for (const UiTarget& target : hits_.targets()) {
                const Rect& r = target.rect;
                if (bar.x < r.x + r.w && r.x < bar.x + bar.w && r.y + r.h > bar.y) {
                    top = std::min(top, r.y);
                }
            }
            bar.y = top - bar_height - margin;
        }
    }
    canvas.fill_rounded_rect(bar, bar_height * 0.5f, hex(0x1a211a, 0.92f));

    float x = bar.x + 10.0f;
    for (const HintItem& item : items) {
        draw_hint_badge(canvas, item.badge, x + badge * 0.5f, bar.center_y(), badge,
                        kForeground, kBackground);
        x += badge + label_gap;
        canvas.text(x, bar.center_y(), label_style, item.label);
        x += canvas.measure_text(label_style, item.label) + item_gap;
    }
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

void CarUi::set_sheet(const PluginSheet& sheet) {
    if (sheet.request_id != sheet_.request_id || sheet.kind != sheet_.kind ||
        sheet.options.size() != sheet_.options.size()) {
        sheet_scroll_row_ = 0;
        sheet_scroll_max_ = 0;
        sheet_scroll_capacity_ = 0;
        sheet_scroll_up_ = {};
        sheet_scroll_down_ = {};
    }
    sheet_ = sheet;
}

bool CarUi::scroll_sheet(int rows) {
    if ((sheet_.kind != PluginSheetKind::Select && sheet_.kind != PluginSheetKind::Confirm) ||
        rows == 0) {
        return false;
    }
    const int next = std::clamp(sheet_scroll_row_ + rows, 0, sheet_scroll_max_);
    if (next == sheet_scroll_row_) {
        return false;
    }
    sheet_scroll_row_ = next;
    return true;
}

float CarUi::update_mic_level(const CallView& call, float time_seconds) {
    const bool connected = call.phase == CallPhase::Active ||
                           call.phase == CallPhase::Speaking ||
                           call.phase == CallPhase::Thinking;
    if (!connected || call.muted || call.input_silenced || call.dictation_paused ||
        call.closing) {
        mic_level_ = 0.0f;
        mic_peak_ = 0.0f;
        mic_last_time_ = time_seconds;
        return mic_level_;
    }

    const float raw = clamp01(call.input_level);
    const bool first = mic_last_time_ < 0.0f || time_seconds + 0.001f < mic_last_time_;
    const float dt = first ? 0.0f : std::clamp(time_seconds - mic_last_time_, 0.0f, 1.0f);
    mic_last_time_ = time_seconds;

    // Peak normalization follows the web's input meter: quiet speech keeps a
    // usable fill, while the floor keeps room noise from pegging the button.
    const float decayed_peak = mic_peak_ * std::exp(-dt / 6.0f);
    mic_peak_ = std::max({raw, decayed_peak, 0.02f});
    const float normalized = std::pow(clamp01(raw / mic_peak_), 0.65f);

    if (first) {
        mic_level_ = normalized;
    } else {
        const float rate = normalized > mic_level_ ? 22.0f : 2.6f;
        mic_level_ += (normalized - mic_level_) * (1.0f - std::exp(-dt * rate));
    }
    mic_level_ = clamp01(mic_level_);
    return mic_level_;
}

float CarUi::advance_stage_light(bool speaking, float time_seconds) {
    const float target = speaking ? 1.0f : 0.0f;
    if (stage_light_time_ < 0.0f || time_seconds + 0.001f < stage_light_time_) {
        stage_light_ = target;
    } else {
        const float dt = std::clamp(time_seconds - stage_light_time_, 0.0f, 0.1f);
        const float rate = speaking ? 14.0f : 2.4f;
        stage_light_ += (target - stage_light_) * (1.0f - std::exp(-dt * rate));
    }
    stage_light_time_ = time_seconds;
    return stage_light_;
}

float CarUi::control_split(float time_seconds, bool in_call) {
    const float target = in_call ? 1.0f : 0.0f;
    if (control_split_ < 0.0f || time_seconds + 0.001f < last_draw_time_) {
        control_split_ = target;
        split_from_ = target;
        split_start_ = time_seconds;
        split_target_ = target;
    } else if (target != split_target_) {
        split_from_ = control_split_;
        split_start_ = time_seconds;
        split_target_ = target;
    }
    const float duration = 0.4f;
    const float progress =
        split_target_ == split_from_
            ? 1.0f
            : clamp01((time_seconds - split_start_) / duration);
    const float eased = cubic_bezier(0.22f, 1.0f, 0.36f, 1.0f, progress);
    control_split_ = split_from_ + (split_target_ - split_from_) * eased;
    last_draw_time_ = time_seconds;
    return control_split_;
}

void CarUi::draw(Canvas& canvas, Size size, float time_seconds) {
    const int caption_height = client_caption_ ? static_cast<int>(kCaptionHeight) : 0;
    const Size content{size.width, std::max(1, size.height - caption_height)};
    last_size_ = content;
    last_draw_time_ = time_seconds;
    if (corner_holding_ && time_seconds - corner_hold_start_ >= 2.0f) {
        debug_revealed_ = true;
        corner_holding_ = false;
    }
    if (debug_was_open_ && !debug_.open) {
        debug_revealed_ = false;
    }
    debug_was_open_ = debug_.open;
    hits_.clear();
    puck_bounds_ = {};
    puck_stage_ = {};
    status_bounds_ = {};
    pairing_retry_bounds_ = {};
    if (caption_height > 0) {
        canvas.save();
        canvas.translate(0.0f, static_cast<float>(caption_height));
    }
    if (debug_.open) {
        draw_debug(canvas, content);
    } else if (call_mode_) {
        draw_call(canvas, content, time_seconds);
    } else {
        draw_pairing(canvas, content, time_seconds);
    }
    draw_focus_overlay(canvas, content);
    if (caption_height > 0) {
        canvas.restore();
        draw_client_caption(canvas, size);
    }
}

void CarUi::draw_client_caption(Canvas& canvas, Size size) {
    canvas.fill_rect(Rect{0.0f, 0.0f, static_cast<float>(size.width), kCaptionHeight},
                     kBackground);
    const TextStyle title_style =
        type(13.0f, kMuted, TextAlign::Left, FontWeight::Medium);
    canvas.text(16.0f, kCaptionHeight * 0.5f, title_style, "Puck Kiosk");

    const float button_y = (kCaptionHeight - kCaptionButtonHeight) * 0.5f;
    const float right = static_cast<float>(size.width) - kCaptionRightMargin;
    const Rect close{right - kCaptionButtonWidth, button_y, kCaptionButtonWidth,
                     kCaptionButtonHeight};
    const Rect maximize{close.x - kCaptionButtonWidth, button_y, kCaptionButtonWidth,
                        kCaptionButtonHeight};
    const Rect minimize{maximize.x - kCaptionButtonWidth, button_y, kCaptionButtonWidth,
                        kCaptionButtonHeight};
    constexpr float kIconSize = 18.0f;
    const auto draw_icon = [&](const char* path, Rect rect) {
        canvas.fill_path(path, kIconBox,
                         icon_rect(rect.center_x(), rect.center_y(), kIconSize), kMuted);
    };
    draw_icon(icons::kMinimize, minimize);
    draw_icon(icons::kMaximize, maximize);
    draw_icon(icons::kClose, close);

    // Pointer input is in content space, so the strip sits just above y=0.
    hits_.add(Rect{0.0f, -kCaptionHeight, static_cast<float>(size.width), kCaptionHeight},
              UiAction::None);
    hits_.add(Rect{minimize.x, minimize.y - kCaptionHeight, minimize.w, minimize.h},
              UiAction::WindowMinimize);
    hits_.add(Rect{maximize.x, maximize.y - kCaptionHeight, maximize.w, maximize.h},
              UiAction::WindowMaximize);
    hits_.add(Rect{close.x, close.y - kCaptionHeight, close.w, close.h},
              UiAction::WindowClose);
}

void CarUi::draw_pairing(Canvas& canvas, Size size, float time_seconds) {
    // A terminal failure with neither a QR nor a confirmation code must not
    // show the white "QR" placeholder: it would look scannable. The waiting
    // screen keeps the placeholder until the first code arrives.
    const bool terminal_without_card =
        code_.empty() && qr_.empty() && pairing_recovery_ != PairingRecovery::None;
    const PairingLayout layout =
        pairing_layout(canvas, size, title_, code_, status_,
                       pairing_recovery_ != PairingRecovery::None, !terminal_without_card);

    const float puck_size = std::min(layout.stage.w, layout.stage.h);
    const float puck_y = layout.portrait ? layout.stage.y + layout.stage.h - puck_size
                                         : layout.stage.y + (layout.stage.h - puck_size) * 0.5f;
    const Rect puck{layout.stage.center_x() - puck_size * 0.5f, puck_y, puck_size, puck_size};
    puck_bounds_ = puck;
    puck_stage_ = layout.stage;
    puck_->set_stage(puck_stage_);
    puck_->draw(canvas, puck, visual_, time_seconds);

    if (!title_.empty()) {
        draw_wrapped_center(canvas, layout.title.center_x(), layout.title.y, layout.title_style,
                            layout.title_line, title_, layout.title.w);
    }

    if (layout.show_card) {
        if (code_.empty()) {
            draw_qr_card(canvas, layout.card, layout.card_radius, qr_);
        } else {
            TextStyle code_style =
                fit_font_size(canvas, layout.code_style, code_, layout.slot.w - 16.0f, 28.0f);
            canvas.text(layout.slot.center_x(), layout.slot.center_y(), code_style, code_);
        }
    }

    if (layout.show_status) {
        status_bounds_ = layout.status;
        const TextStyle status_style = layout.status_style;
        canvas.text(layout.status.center_x(), layout.status.center_y(), status_style,
                    fit_text(canvas, status_style, status_, layout.status.w));
    }

    if (layout.show_retry) {
        pairing_retry_bounds_ = layout.retry;
        const bool retrying = pairing_recovery_ == PairingRecovery::Retrying;
        canvas.fill_rounded_rect(layout.retry, layout.retry.h * 0.5f,
                                 retrying ? hex(0x1a211a) : kSecondary);
        const TextStyle retry_style =
            retrying ? type(layout.retry_style.size, kMuted, TextAlign::Center,
                            FontWeight::Medium)
                     : layout.retry_style;
        canvas.text(layout.retry.center_x(), layout.retry.center_y(), retry_style,
                    retrying ? "Retrying\u2026" : "Try Again");
        if (!retrying) {
            add_target(layout.retry, UiAction::PairingRetry);
        }
    }

    add_debug_target(canvas, size, true);
}

void CarUi::draw_call(Canvas& canvas, Size size, float time_seconds) {
    const CallLayout layout = call_layout(size);
    const bool in_call = call_.in_call() || call_.phase == CallPhase::Failed;
    const float split = control_split(time_seconds, in_call);
    mic_level_ = update_mic_level(call_, time_seconds);
    const float stage_light = advance_stage_light(visual_.speaking, time_seconds);

    draw_call_header(canvas, layout, size);

    if (passkey_.mode != PasskeyMode::Hidden && !in_call) {
        draw_passkey(canvas, layout.main_content, layout.compact);
    } else {
        if (!puck_->is_3d()) {
            draw_call_backdrop(canvas, layout.main, layout.puck, time_seconds, stage_light);
        }
        // The actor's aligned PuckVisual is the only source of the stage:
        // CallView supplies copy and local input, not a second opinion about
        // speaking, listening, thinking, or mouth.
        puck_bounds_ = layout.puck;
        puck_stage_ = layout.main;
        puck_->set_stage(puck_stage_);
        puck_->draw(canvas, layout.puck, visual_, time_seconds);
        draw_call_caption(canvas, layout, call_);
    }

    if (media_ && media_->visible()) {
        media_->draw(canvas, layout.overlay, layout.compact);
        const MediaButtons buttons = media_->buttons(canvas, layout.overlay, layout.compact);
        add_target(buttons.close, UiAction::MediaClose);
        if (buttons.can_previous) {
            add_target(buttons.previous, UiAction::MediaPrevious);
        }
        if (buttons.can_next) {
            add_target(buttons.next, UiAction::MediaNext);
        }
    }

    if (sheet_.kind != PluginSheetKind::None) {
        draw_sheet(canvas, layout.overlay, layout.compact);
    }

    draw_call_controls(canvas, layout, call_, passkey_, split, mic_level_, call_available_,
                       hits_);
    add_debug_target(canvas, size, false);
}

void CarUi::draw_passkey(Canvas& canvas, Rect area, bool compact) {
    if (passkey_.title.empty() && passkey_.steps.empty()) {
        return;
    }
    const float pad_x = compact ? 24.0f : 40.0f;
    const float content_width = std::min(area.w - pad_x * 2.0f, 896.0f);
    const float left = area.center_x() - content_width * 0.5f;

    const float title_size = compact ? 40.0f : 48.0f;
    const float title_line = compact ? 50.0f : 60.0f;
    const TextStyle title_style =
        type(title_size, kForeground, TextAlign::Center, FontWeight::Semibold, -0.025f);
    const float section_gap = compact ? 28.0f : 40.0f;
    const float steps_gap = compact ? 24.0f : 28.0f;
    const float number_size = compact ? 48.0f : 64.0f;
    const float step_size = compact ? 28.0f : 40.0f;
    const float step_line = compact ? 34.0f : 40.0f;
    const float detail_size = compact ? 22.0f : 24.0f;
    const float detail_line = compact ? 30.0f : 33.0f;
    const TextStyle step_style = type(step_size, kForeground, TextAlign::Left, FontWeight::Medium);
    const TextStyle detail_style = type(detail_size, kMuted, TextAlign::Left);
    const TextStyle number_style =
        type(compact ? 24.0f : 30.0f, kForeground, TextAlign::Center, FontWeight::Semibold);

    const float text_width = content_width - number_size - 24.0f;
    float total = 0.0f;
    std::vector<float> step_heights;
    std::vector<std::vector<std::string>> step_titles;
    std::vector<std::vector<std::string>> step_details;
    for (const PasskeyStep& step : passkey_.steps) {
        const std::vector<std::string> titles =
            wrap_text(canvas, step_style, step.title, text_width);
        const std::vector<std::string> details =
            step.detail.empty() ? std::vector<std::string>{}
                                : wrap_text(canvas, detail_style, step.detail, text_width);
        const float text_height = 8.0f + static_cast<float>(titles.size()) * step_line +
                                  (details.empty()
                                       ? 0.0f
                                       : 4.0f + static_cast<float>(details.size()) * detail_line);
        step_heights.push_back(std::max(number_size, text_height));
        step_titles.push_back(titles);
        step_details.push_back(details);
        total += step_heights.back();
    }
    total += static_cast<float>(step_heights.size() > 0 ? step_heights.size() - 1 : 0) * steps_gap;
    const float title_height =
        static_cast<float>(wrap_text(canvas, title_style, passkey_.title, content_width).size()) *
        title_line;
    total += title_height + section_gap;
    if (!passkey_.status.empty()) {
        total += 12.0f + 24.0f;
    }

    float top = area.center_y() - total * 0.5f;
    top = std::max(area.y, top);
    draw_wrapped_center(canvas, area.center_x(), top, title_style, title_line, passkey_.title,
                        content_width);
    float y = top + title_height + section_gap;
    for (std::size_t i = 0; i < passkey_.steps.size(); ++i) {
        const float number_center_y = y + number_size * 0.5f;
        canvas.fill_circle(left + number_size * 0.5f, number_center_y, number_size * 0.5f,
                           kSecondary);
        canvas.text(left + number_size * 0.5f, number_center_y, number_style,
                    std::to_string(i + 1));
        const float text_left = left + number_size + 24.0f;
        float text_y = y + 8.0f;
        for (const std::string& line : step_titles[i]) {
            text_in_line(canvas, text_left, text_y, step_line, step_style, line);
            text_y += step_line;
        }
        if (!step_details[i].empty()) {
            text_y += 4.0f;
            for (const std::string& line : step_details[i]) {
                text_in_line(canvas, text_left, text_y, detail_line, detail_style, line);
                text_y += detail_line;
            }
        }
        y += step_heights[i] + steps_gap;
    }
    if (!passkey_.status.empty()) {
        const Color color = passkey_.mode == PasskeyMode::Failed ? kDestructive : kMuted;
        canvas.text(area.center_x(), y + 12.0f, type(17.0f, color, TextAlign::Center),
                    fit_text(canvas, type(17.0f, color), passkey_.status, content_width));
    }
}

void CarUi::draw_sheet(Canvas& canvas, Rect area, bool compact) {
    add_target(area, UiAction::None);
    canvas.fill_rect(area, kBackground);

    const float pad_x = compact ? 16.0f : 40.0f;
    const float content_width = std::min(area.w - pad_x * 2.0f, 896.0f);
    const float left = area.center_x() - content_width * 0.5f;
    const float gap = compact ? 20.0f : 32.0f;

    if (sheet_.kind == PluginSheetKind::Phone) {
        const float icon_size = compact ? 48.0f : 64.0f;
        const float title_size = compact ? 24.0f : 40.0f;
        const float title_line = compact ? 32.0f : 40.0f;
        const float subtitle_size = compact ? 18.0f : 24.0f;
        const float subtitle_line = compact ? 24.0f : 32.0f;
        const float ok_height = compact ? 48.0f : 80.0f;
        const float ok_size = compact ? 18.0f : 24.0f;
        const float ok_pad = compact ? 24.0f : 56.0f;
        const TextStyle title_style =
            type(title_size, kForeground, TextAlign::Center, FontWeight::Semibold);
        const TextStyle subtitle_style = type(subtitle_size, kMuted, TextAlign::Center);
        const std::vector<std::string> subtitles =
            wrap_text(canvas, subtitle_style, sheet_.title, content_width);
        const float subtitle_height =
            static_cast<float>(subtitles.size()) * subtitle_line;
        const float ok_width =
            std::min(content_width, canvas.measure_text(type(ok_size, kWhite), "OK") + ok_pad * 2.0f);
        const float total = icon_size + gap + title_line + gap + subtitle_height + gap + ok_height;
        float y = area.center_y() - total * 0.5f;
        draw_call_icon(canvas, area.center_x(), y + icon_size * 0.5f, icon_size, kMuted);
        y += icon_size + gap;
        text_in_line(canvas, area.center_x(), y, title_line, title_style, "Answer on Your Phone");
        y += title_line + gap;
        for (const std::string& line : subtitles) {
            text_in_line(canvas, area.center_x(), y, subtitle_line, subtitle_style, line);
            y += subtitle_line;
        }
        y += gap;
        const Rect ok{area.center_x() - ok_width * 0.5f, y, ok_width, ok_height};
        canvas.fill_rounded_rect(ok, ok_height * 0.5f, kSecondary);
        canvas.text(ok.center_x(), ok.center_y(), type(ok_size, kForeground, TextAlign::Center),
                    "OK");
        add_target(ok, UiAction::SheetOk);
        return;
    }

    const bool confirm = sheet_.kind == PluginSheetKind::Confirm;
    const std::size_t count = confirm ? 2 : sheet_.options.size();
    const bool dense = count > 4;
    const float preferred_option_height =
        compact ? (dense ? 48.0f : 64.0f) : (dense ? 64.0f : 96.0f);
    const float option_gap = dense ? 12.0f : 20.0f;
    const int columns = 2;
    const int rows = static_cast<int>((count + columns - 1) / columns);
    const float title_size = compact ? 24.0f : 40.0f;
    const float title_line = compact ? 32.0f : 40.0f;
    const float eyebrow_size = compact ? 18.0f : 24.0f;
    const float eyebrow_line = compact ? 24.0f : 32.0f;
    const float dismiss_height = compact ? 48.0f : 64.0f;
    const float dismiss_size = compact ? 18.0f : 24.0f;
    const TextStyle title_style =
        type(title_size, kForeground, TextAlign::Center, FontWeight::Semibold);
    const TextStyle eyebrow_style = type(eyebrow_size, kMuted, TextAlign::Center);
    const TextStyle option_style = type(
        compact ? (dense ? 18.0f : 20.0f) : (dense ? 24.0f : 30.0f), kForeground,
        TextAlign::Center, FontWeight::Medium);

    bool has_eyebrow = !sheet_.message.empty();
    std::string eyebrow = has_eyebrow ? sheet_.title : std::string{};
    std::string heading = has_eyebrow ? sheet_.message : sheet_.title;
    std::vector<std::string> headings = wrap_text(canvas, title_style, heading, content_width);
    float header_height =
        (has_eyebrow ? eyebrow_line + 12.0f : 0.0f) +
        static_cast<float>(std::max<std::size_t>(1, headings.size())) * title_line;

    auto draw_header = [&](float top) {
        float y = top;
        if (has_eyebrow) {
            text_in_line(canvas, area.center_x(), y, eyebrow_line, eyebrow_style,
                         fit_text(canvas, eyebrow_style, eyebrow, content_width));
            y += eyebrow_line + 12.0f;
        }
        for (const std::string& line : headings) {
            text_in_line(canvas, area.center_x(), y, title_line, title_style, line);
            y += title_line;
        }
    };

    const TextStyle dismiss_style = type(dismiss_size, kMuted, TextAlign::Center);
    const float dismiss_width =
        std::min(content_width, canvas.measure_text(dismiss_style, "Dismiss") + 80.0f);
    const float dismiss_bottom = area.y + area.h - 8.0f;

    // Two columns when the whole set fits. A long list switches to one
    // scrolling column instead of compressing rows until they collide.
    const float grid_height =
        rows > 0 ? rows * preferred_option_height + (rows - 1) * option_gap : 0.0f;
    const float grid_total = header_height + gap + grid_height + gap + dismiss_height;
    if (grid_total <= area.h - 16.0f) {
        float top = area.y + (area.h - grid_total) * 0.5f;
        draw_header(top);
        const float y = top + header_height + gap;
        const float button_width = (content_width - option_gap * (columns - 1)) / columns;
        for (std::size_t i = 0; i < count; ++i) {
            const int column = static_cast<int>(i) % columns;
            const int row = static_cast<int>(i) / columns;
            const Rect button{left + static_cast<float>(column) * (button_width + option_gap),
                              y + static_cast<float>(row) * (preferred_option_height + option_gap),
                              button_width, preferred_option_height};
            const std::string label = confirm ? (i == 0 ? sheet_.confirm_label : "No")
                                              : sheet_.options[i];
            canvas.fill_rounded_rect(button, 16.0f, kSecondary);
            draw_option_label(canvas, button, option_style, label);
            add_target(button, UiAction::SheetOption, static_cast<int>(i));
        }
        const Rect dismiss{area.center_x() - dismiss_width * 0.5f, y + grid_height + gap,
                           dismiss_width, dismiss_height};
        canvas.text(dismiss.center_x(), dismiss.center_y(), dismiss_style, "Dismiss");
        add_target(dismiss, UiAction::SheetDismiss);
        sheet_scroll_row_ = 0;
        sheet_scroll_max_ = 0;
        sheet_scroll_capacity_ = 0;
        sheet_scroll_up_ = {};
        sheet_scroll_down_ = {};
        return;
    }

    // Scroll mode pins the header to the top and Dismiss to the bottom, so the
    // list owns the remaining height and the final option stays reachable.
    float row_height = compact ? 48.0f : 64.0f;
    const float row_gap = 12.0f;
    const float dismiss_top = dismiss_bottom - dismiss_height;
    float header_top = area.y + 8.0f;
    float list_top = header_top + header_height + gap;
    float list_bottom = dismiss_top - gap;
    if (list_bottom - list_top < row_height) {
        // A very short window gives the list priority over the eyebrow and
        // extra heading lines; Dismiss and one option row remain visible.
        has_eyebrow = false;
        if (headings.size() > 1) {
            headings.resize(1);
        }
        header_height = headings.empty() ? 0.0f : title_line;
        list_top = header_top + header_height + gap;
    }
    if (list_bottom - list_top < row_height) {
        list_top = header_top + header_height + 8.0f;
        list_bottom = dismiss_top - 8.0f;
    }
    const float list_height = std::max(0.0f, list_bottom - list_top);
    row_height = std::min(row_height, std::max(32.0f, list_height));
    const int capacity = std::max(
        1, static_cast<int>((list_height + row_gap) / (row_height + row_gap)));
    sheet_scroll_capacity_ = capacity;
    sheet_scroll_max_ = std::max(0, static_cast<int>(count) - capacity);
    sheet_scroll_row_ = std::clamp(sheet_scroll_row_, 0, sheet_scroll_max_);

    draw_header(header_top);

    const float rail_width =
        std::min(compact ? 40.0f : 44.0f, std::max(20.0f, content_width * 0.22f));
    const float rail_gap =
        std::min(compact ? 8.0f : 12.0f, std::max(2.0f, content_width * 0.04f));
    const float option_width = std::max(24.0f, content_width - rail_width - rail_gap);
    const float rail_x = left + content_width - rail_width;
    const int first = sheet_scroll_row_;
    const int last = std::min(static_cast<int>(count), first + capacity);
    for (int i = first; i < last; ++i) {
        const Rect button{left, list_top + static_cast<float>(i - first) * (row_height + row_gap),
                          option_width, row_height};
        const std::string label = confirm
                                      ? (i == 0 ? sheet_.confirm_label : "No")
                                      : sheet_.options[static_cast<std::size_t>(i)];
        canvas.fill_rounded_rect(button, 16.0f, kSecondary);
        draw_option_label(canvas, button, option_style, label);
        add_target(button, UiAction::SheetOption, i);
    }

    // The rail is pointer/touch scrolling. Its targets own the tap as
    // SheetScroll actions, so a tap can never fall through to an option; the
    // bumpers page the list for a gamepad, and the keyboard handles arrows,
    // Page keys, Home, and End.
    const float rail_button_height = std::min(compact ? 44.0f : 48.0f, list_height * 0.5f);
    const Rect up{rail_x, list_top, rail_width, rail_button_height};
    const Rect down{rail_x, list_bottom - rail_button_height, rail_width, rail_button_height};
    const bool can_up = sheet_scroll_row_ > 0;
    const bool can_down = sheet_scroll_row_ < sheet_scroll_max_;
    const auto draw_rail_button = [&](Rect rect, bool down_arrow, bool enabled, UiAction action) {
        canvas.fill_rounded_rect(rect, rect.h * 0.5f,
                                 fade(kSecondary, enabled ? 0.95f : 0.35f));
        draw_chevron(canvas, rect.center_x(), rect.center_y(), rect.h * 0.32f, down_arrow,
                     fade(enabled ? kForeground : kMuted, enabled ? 1.0f : 0.45f));
        add_target(rect, action);
    };
    draw_rail_button(up, false, can_up, UiAction::SheetScrollUp);
    draw_rail_button(down, true, can_down, UiAction::SheetScrollDown);
    const float track_top = up.y + up.h + 6.0f;
    const float track_bottom = down.y - 6.0f;
    if (track_bottom > track_top) {
        const float rail_center = rail_x + rail_width * 0.5f;
        canvas.stroke_line(rail_center, track_top, rail_center, track_bottom, 2.0f,
                           hex(0x2f372f, 0.9f));
        const float track_height = track_bottom - track_top;
        const float thumb_height =
            std::max(20.0f, track_height * static_cast<float>(capacity) /
                                static_cast<float>(std::max(1, static_cast<int>(count))));
        const float travel = std::max(0.0f, track_height - thumb_height);
        const float progress = sheet_scroll_max_ > 0
                                   ? static_cast<float>(sheet_scroll_row_) /
                                         static_cast<float>(sheet_scroll_max_)
                                   : 0.0f;
        canvas.fill_rounded_rect(
            Rect{rail_center - 2.0f, track_top + travel * progress, 4.0f, thumb_height}, 2.0f,
            hex(0x9ca49c, 0.65f));
    }
    sheet_scroll_up_ = up;
    sheet_scroll_down_ = down;

    const Rect dismiss{area.center_x() - dismiss_width * 0.5f, dismiss_top, dismiss_width,
                       dismiss_height};
    canvas.text(dismiss.center_x(), dismiss.center_y(), dismiss_style, "Dismiss");
    add_target(dismiss, UiAction::SheetDismiss);
}

void CarUi::draw_debug(Canvas& canvas, Size size) {
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    const bool compact = compact_layout(size);
    const float pad = compact ? 16.0f : 40.0f;
    const float bar_height = compact ? 56.0f : 96.0f;
    const float heading_size = compact ? 20.0f : 30.0f;
    const float close_height = compact ? 44.0f : 80.0f;
    const float close_size = compact ? 16.0f : 24.0f;
    const float close_pad = compact ? 20.0f : 48.0f;
    const float row_height = compact ? 44.0f : 64.0f;
    const float row_size = compact ? 16.0f : 20.0f;
    const float pill_pad = compact ? 16.0f : 28.0f;
    const float action_gap = compact ? 12.0f : 32.0f;

    canvas.fill_rect(Rect{0.0f, 0.0f, width, height}, kBackground);
    canvas.text(pad + kSafeLeft, bar_height * 0.5f,
                type(heading_size, kForeground, TextAlign::Left, FontWeight::Semibold), "Debug");

    const TextStyle close_style = type(close_size, kForeground, TextAlign::Center,
                                       FontWeight::Medium);
    const float close_width =
        canvas.measure_text(close_style, "Close") + close_pad * 2.0f;
    const Rect close{width - pad - kSafeRight - close_width, (bar_height - close_height) * 0.5f,
                     close_width, close_height};
    canvas.fill_rounded_rect(close, close_height * 0.5f, kSecondary);
    canvas.text(close.center_x(), close.center_y(), close_style, "Close");
    add_target(close, UiAction::DebugClose);

    const TextStyle label_style = type(row_size, kMuted);
    const TextStyle pill_style = type(row_size, kForeground, TextAlign::Center,
                                      FontWeight::Medium);
    float x = pad + kSafeLeft;
    float row_top = bar_height;
    const float row_center = row_top + row_height * 0.5f;
    canvas.text(x, row_center, label_style, "Echo Cancellation");
    x += canvas.measure_text(label_style, "Echo Cancellation") + 12.0f;

    struct ModePill {
        const char* label;
        EchoCancellationMode mode;
    };
    const ModePill modes[] = {
        {"On", EchoCancellationMode::On},
        {"Remote Only", EchoCancellationMode::RemoteOnly},
        {"Off", EchoCancellationMode::Off},
    };
    for (const ModePill& mode : modes) {
        const float pill_width = canvas.measure_text(pill_style, mode.label) + pill_pad * 2.0f;
        const Rect pill{x, row_top, pill_width, row_height};
        const bool active = debug_.echo == mode.mode;
        canvas.fill_rounded_rect(pill, row_height * 0.5f, active ? kForeground : kSecondary);
        canvas.text(pill.center_x(), pill.center_y(),
                    type(row_size, active ? kBackground : kForeground, TextAlign::Center,
                         FontWeight::Medium),
                    mode.label);
        add_target(pill, UiAction::EchoMode, static_cast<int>(&mode - modes));
        x += pill_width + 12.0f;
    }

    x += action_gap;
    const float mic_width = canvas.measure_text(pill_style, "Test Microphone") + 32.0f * 2.0f;
    const Rect mic{x, row_top, mic_width, row_height};
    if (debug_.mic_busy) {
        canvas.fill_rounded_rect(mic, row_height * 0.5f, hex(0x1a211a));
        canvas.text(mic.center_x(), mic.center_y(),
                    type(row_size, kMuted, TextAlign::Center, FontWeight::Medium),
                    "Test Microphone");
    } else {
        canvas.fill_rounded_rect(mic, row_height * 0.5f, kSecondary);
        canvas.text(mic.center_x(), mic.center_y(), pill_style, "Test Microphone");
        add_target(mic, UiAction::MicTest);
    }
    x += mic_width + action_gap;

    if (debug_.can_send) {
        const float send_width =
            canvas.measure_text(pill_style, "Send Diagnostics") + 32.0f * 2.0f;
        const Rect send{x, row_top, send_width, row_height};
        canvas.fill_rounded_rect(send, row_height * 0.5f, kSecondary);
        canvas.text(send.center_x(), send.center_y(), pill_style, "Send Diagnostics");
        add_target(send, UiAction::SendDiagnostics);
        x += send_width + action_gap;
    }

    const std::string status =
        !debug_.can_send ? "Sign in to send"
        : debug_.send_status.empty() ? "Echo setting applies to the next call"
                                     : debug_.send_status;
    const TextStyle status_style = type(row_size, kMuted);
    const float status_width = canvas.measure_text(status_style, status);
    if (x + status_width > width - pad - kSafeRight) {
        row_top += row_height + (compact ? 12.0f : 16.0f);
        x = pad + kSafeLeft;
    }
    canvas.text(x, row_top + row_height * 0.5f, status_style,
                fit_text(canvas, status_style, status, width - pad - x));

    const float rows_top = row_top + row_height + (compact ? 16.0f : 24.0f);
    const float label_width = compact ? 120.0f : 208.0f;
    const float value_gap = compact ? 12.0f : 24.0f;
    const float value_x = pad + label_width + value_gap;
    const float debug_row_line = compact ? 20.0f : 28.0f;
    const float debug_row_height = compact ? 28.0f : 40.0f;
    const float debug_row_size = compact ? 14.0f : 20.0f;
    const int max_rows = std::max(
        0, static_cast<int>((height - rows_top - 8.0f) / debug_row_height));
    const int rows = static_cast<int>(debug_.rows.size());
    const int shown = std::min(rows, max_rows);
    for (int i = 0; i < shown; ++i) {
        const DebugRow& row = debug_.rows[static_cast<std::size_t>(i)];
        const float y = rows_top + static_cast<float>(i) * debug_row_height;
        const TextStyle label = type(debug_row_size, kMuted);
        const TextStyle value = type(debug_row_size, kForeground);
        text_in_line(canvas, pad + kSafeLeft, y, debug_row_line, label, row.label);
        text_in_line(canvas, value_x, y, debug_row_line, value,
                     fit_text(canvas, value, row.value, width - value_x - pad - kSafeRight));
    }
    if (shown < rows) {
        const float y = rows_top + static_cast<float>(shown) * debug_row_height;
        canvas.text(pad + kSafeLeft, y + debug_row_line * 0.5f,
                    type(compact ? 13.0f : 16.0f, kMuted),
                    "+" + std::to_string(rows - shown) + " more");
    }
}

void CarUi::add_debug_target(Canvas& canvas, Size size, bool pairing) {
    if (!debug_revealed_ && !debug_.open) {
        return;
    }
    const bool compact = compact_layout(size);
    const float debug_height = compact ? 44.0f : 64.0f;
    const float debug_font = compact ? 16.0f : 20.0f;
    const float debug_pad = compact ? 16.0f : 24.0f;
    const float margin = compact ? 12.0f : (pairing ? 40.0f : 24.0f);
    const TextStyle label_style =
        type(debug_font, kMuted, TextAlign::Center, FontWeight::Medium);
    const float debug_width = canvas.measure_text(label_style, "Debug") + debug_pad * 2.0f;
    const Rect entry{margin + kSafeLeft, static_cast<float>(size.height) - margin - debug_height -
                                              kSafeBottom,
                     debug_width, debug_height};
    canvas.fill_rounded_rect(entry, debug_height * 0.5f, hex(0x2f372f, 0.65f));
    canvas.text(entry.center_x(), entry.center_y(), label_style, "Debug");
    add_target(entry, UiAction::DebugOpen);
}

}  // namespace pk
