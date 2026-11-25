#include "app/car_ui.h"
#include "app/plugin_ui.h"
#include "platform/headless/headless_display.h"
#include "puck/puck_view.h"
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

bool read_ppm(const std::string& path, Image* image) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::string magic;
    int max_value = 0;
    input >> magic >> image->width >> image->height >> max_value;
    if (magic != "P6" || image->width <= 0 || image->height <= 0 || max_value != 255) {
        return false;
    }
    input.get();
    image->rgb.resize(static_cast<std::size_t>(image->width) * image->height * 3);
    input.read(reinterpret_cast<char*>(image->rgb.data()),
               static_cast<std::streamsize>(image->rgb.size()));
    return input.good() || input.eof();
}

class FakePuck final : public pk::PuckView {
public:
    const char* name() const override { return "fake"; }
    bool is_3d() const override { return false; }
    bool initialize(std::string*) override { return true; }
    void draw(pk::Canvas&, pk::Rect, const pk::PuckVisual&, float) override {}
};

struct InkBounds {
    bool found = false;
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
    int pixels = 0;
};

InkBounds ink_bounds(const Image& image, const pk::Rect& rect, int threshold = 140) {
    InkBounds bounds;
    const int x0 = std::max(0, static_cast<int>(std::floor(rect.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(rect.y)));
    const int x1 = std::min(image.width - 1, static_cast<int>(std::ceil(rect.x + rect.w)));
    const int y1 = std::min(image.height - 1, static_cast<int>(std::ceil(rect.y + rect.h)));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 3;
            const int red = image.rgb[index];
            const int green = image.rgb[index + 1];
            const int blue = image.rgb[index + 2];
            // The option fill is #2f372f (about 55 luma) and the label is
            // #f6fff5 (about 250). Anti-aliased glyph edges cross the midpoint.
            // The caption's 1px minimize bar needs a lower threshold.
            const int luma = (red * 299 + green * 587 + blue * 114) / 1000;
            if (luma < threshold) {
                continue;
            }
            if (!bounds.found) {
                bounds.found = true;
                bounds.min_x = bounds.max_x = x;
                bounds.min_y = bounds.max_y = y;
            } else {
                bounds.min_x = std::min(bounds.min_x, x);
                bounds.max_x = std::max(bounds.max_x, x);
                bounds.min_y = std::min(bounds.min_y, y);
                bounds.max_y = std::max(bounds.max_y, y);
            }
            ++bounds.pixels;
        }
    }
    return bounds;
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb pixel_at(const Image& image, int x, int y) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 3;
    return Rgb{image.rgb[index], image.rgb[index + 1], image.rgb[index + 2]};
}

bool is_background(Rgb color) {
    // #0b0d0b, the app background token in car_ui.cpp.
    return color.r == 0x0b && color.g == 0x0d && color.b == 0x0b;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "ui-render-tests: %s\n", message.c_str());
    return 1;
}

}  // namespace

int main() {
    std::error_code directory_error;
    std::filesystem::create_directories("build", directory_error);
    const std::string output = "build/ui-render-test.ppm";
    pk::HeadlessDisplay display(1280, 720, output, 1);
    std::string error;
    if (!display.initialize(&error)) {
        return fail("headless display: " + error);
    }
    display.make_current();

    pk::Renderer renderer;
    if (!renderer.initialize("", &error)) {
        return fail("renderer: " + error);
    }

    // Puck3D reads the layout-to-surface factor through the Canvas seam. It
    // must follow begin_frame and survive the flush that submits the 2D layer.
    renderer.begin(display.size(), 2.0f);
    if (renderer.canvas().pixel_scale() != 2.0f) {
        return fail("canvas pixel_scale does not follow begin_frame");
    }
    renderer.canvas().flush();
    if (renderer.canvas().pixel_scale() != 2.0f) {
        return fail("canvas pixel_scale does not survive flush");
    }
    renderer.end();
    std::printf("PASS: canvas reports the supersample pixel scale\n");

    FakePuck puck;
    pk::CarUi ui(puck);
    ui.set_call(pk::CallView{});
    pk::PluginSheet sheet;
    sheet.kind = pk::PluginSheetKind::Select;
    sheet.title = "Pick a track";
    sheet.options = {"Play"};
    ui.set_sheet(sheet);

    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 0.0f);
    renderer.end();
    display.swap();

    const pk::UiTarget* option = nullptr;
    for (const pk::UiTarget& target : ui.hit_map().targets()) {
        if (target.action == pk::UiAction::SheetOption) {
            option = &target;
            break;
        }
    }
    if (!option) {
        return fail("the select sheet did not expose an option rect");
    }

    Image image;
    if (!read_ppm(output, &image)) {
        return fail("cannot read " + output);
    }
    const InkBounds ink = ink_bounds(image, option->rect);
    if (!ink.found || ink.pixels < 40) {
        return fail("the option label produced no readable ink");
    }

    const float ink_center_y = (static_cast<float>(ink.min_y) + ink.max_y) * 0.5f;
    const float ink_center_x = (static_cast<float>(ink.min_x) + ink.max_x) * 0.5f;
    const float expected_y = option->rect.center_y();
    const float expected_x = option->rect.center_x();
    std::printf("option rect %.1f,%.1f %.1fx%.1f; ink x %.1f..%d y %d..%d (%d px)\n",
                option->rect.x, option->rect.y, option->rect.w, option->rect.h,
                static_cast<double>(ink_center_x), ink.max_x, ink.min_y, ink.max_y, ink.pixels);

    int failures = 0;
    if (std::fabs(ink_center_y - expected_y) > 3.0f) {
        std::fprintf(stderr,
                     "label ink center y %.2f is not within 3px of the button center %.2f\n",
                     static_cast<double>(ink_center_y), static_cast<double>(expected_y));
        ++failures;
    }
    if (std::fabs(ink_center_x - expected_x) > 3.0f) {
        std::fprintf(stderr,
                     "label ink center x %.2f is not within 3px of the button center %.2f\n",
                     static_cast<double>(ink_center_x), static_cast<double>(expected_x));
        ++failures;
    }
    if (ink.min_y < option->rect.y + 8.0f || ink.max_y > option->rect.y + option->rect.h - 8.0f) {
        std::fprintf(stderr, "label ink touches the button edge: y %d..%d in %.1f..%.1f\n",
                     ink.min_y, ink.max_y, static_cast<double>(option->rect.y),
                     static_cast<double>(option->rect.y + option->rect.h));
        ++failures;
    }
    // A muted call must show its caption line (live transcripts use it too).
    ui.set_sheet({});
    pk::CallView muted;
    muted.phase = pk::CallPhase::Muted;
    muted.caption = "Microphone off";
    ui.set_call(muted);
    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 1.0f);
    renderer.end();
    display.swap();
    if (!read_ppm(output, &image)) {
        return fail("cannot read the muted-call frame");
    }
    const InkBounds caption = ink_bounds(image, pk::Rect{400.0f, 640.0f, 480.0f, 60.0f});
    if (!caption.found || caption.pixels < 40) {
        std::fprintf(stderr, "the muted call drew no caption ink\n");
        ++failures;
    } else {
        std::printf("PASS: muted call shows its caption\n");
    }

    // The Mute button carries the local input meter: a green fill when the
    // microphone is live, and none when muted.
    ui.set_sheet({});
    pk::CallView mic;
    mic.phase = pk::CallPhase::Active;
    mic.input_level = 0.05f;
    mic.caption = "On a call with Puck";
    ui.set_call(mic);
    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 4.0f);
    renderer.end();
    display.swap();
    if (!read_ppm(output, &image)) {
        return fail("cannot read the mic-meter frame");
    }
    const pk::UiTarget* mute = nullptr;
    for (const pk::UiTarget& target : ui.hit_map().targets()) {
        if (target.action == pk::UiAction::Mute) {
            mute = &target;
            break;
        }
    }
    if (!mute) {
        return fail("the active call has no Mute button");
    }
    int meter_pixels = 0;
    const int meter_x0 = static_cast<int>(mute->rect.x + mute->rect.w * 0.25f);
    const int meter_x1 = static_cast<int>(mute->rect.x + mute->rect.w * 0.75f);
    const int meter_y0 = static_cast<int>(mute->rect.y + mute->rect.h * 0.72f);
    const int meter_y1 = static_cast<int>(mute->rect.y + mute->rect.h * 0.92f);
    for (int y = meter_y0; y <= meter_y1; ++y) {
        for (int x = meter_x0; x <= meter_x1; ++x) {
            const Rgb color = pixel_at(image, x, y);
            if (color.g > color.r + 20 && color.g > color.b + 12) {
                ++meter_pixels;
            }
        }
    }
    if (meter_pixels < 100) {
        std::fprintf(stderr, "the live microphone fill drew only %d green pixels\n",
                     meter_pixels);
        ++failures;
    } else {
        std::printf("PASS: live microphone fill renders (%d px)\n", meter_pixels);
    }

    mic.phase = pk::CallPhase::Muted;
    mic.muted = true;
    ui.set_call(mic);
    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 4.2f);
    renderer.end();
    display.swap();
    if (!read_ppm(output, &image)) {
        return fail("cannot read the muted-meter frame");
    }
    int muted_meter_pixels = 0;
    for (int y = meter_y0; y <= meter_y1; ++y) {
        for (int x = meter_x0; x <= meter_x1; ++x) {
            const Rgb color = pixel_at(image, x, y);
            if (color.g > color.r + 20 && color.g > color.b + 12) {
                ++muted_meter_pixels;
            }
        }
    }
    if (muted_meter_pixels != 0) {
        std::fprintf(stderr, "the muted microphone kept %d fill pixels\n",
                     muted_meter_pixels);
        ++failures;
    } else {
        std::printf("PASS: muted microphone clears the fill\n");
    }

    // Quiet speech still lights the bottom of the meter.
    mic.phase = pk::CallPhase::Active;
    mic.muted = false;
    mic.input_level = 0.004f;
    ui.set_call(mic);
    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 5.0f);
    renderer.end();
    display.swap();
    if (!read_ppm(output, &image)) {
        return fail("cannot read the quiet-meter frame");
    }
    int quiet_meter_pixels = 0;
    for (int y = meter_y0; y <= meter_y1; ++y) {
        for (int x = meter_x0; x <= meter_x1; ++x) {
            const Rgb color = pixel_at(image, x, y);
            if (color.g > color.r + 20 && color.g > color.b + 12) {
                ++quiet_meter_pixels;
            }
        }
    }
    if (quiet_meter_pixels < 100) {
        std::fprintf(stderr, "the quiet microphone fill drew only %d green pixels\n",
                     quiet_meter_pixels);
        ++failures;
    } else {
        std::printf("PASS: quiet microphone fill stays visible (%d px)\n",
                    quiet_meter_pixels);
    }

    // Client caption chrome: windowed Win32 has no OS title bar, so the app
    // draws this strip and translates the screen content below it.
    ui.set_client_caption(true);
    ui.set_sheet({});
    pk::CallView active;
    active.phase = pk::CallPhase::Active;
    active.caption = "On a call with Puck";
    ui.set_call(active);
    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 2.0f);
    renderer.end();
    display.swap();
    if (!read_ppm(output, &image)) {
        return fail("cannot read the client-caption frame");
    }

    int caption_background_failures = 0;
    for (int y = 2; y <= 34; ++y) {
        for (int x = 200; x <= 1100; ++x) {
            if (!is_background(pixel_at(image, x, y))) {
                if (caption_background_failures == 0) {
                    const Rgb color = pixel_at(image, x, y);
                    std::fprintf(stderr, "caption background is %d,%d,%d at %d,%d\n",
                                 color.r, color.g, color.b, x, y);
                }
                ++caption_background_failures;
            }
        }
    }
    if (caption_background_failures != 0) {
        std::fprintf(stderr, "caption background mismatched %d pixels\n",
                     caption_background_failures);
        ++failures;
    } else {
        std::printf("PASS: client caption background is exactly #0b0d0b\n");
    }

    const pk::Rect title_rect{8.0f, 4.0f, 160.0f, 28.0f};
    const InkBounds title_ink = ink_bounds(image, title_rect);
    if (!title_ink.found || title_ink.pixels < 20) {
        std::fprintf(stderr, "client caption title drew no ink\n");
        ++failures;
    } else if (title_ink.min_x < 8 || title_ink.min_x > 40) {
        std::fprintf(stderr, "client caption title is not left-aligned: x %d..%d\n",
                     title_ink.min_x, title_ink.max_x);
        ++failures;
    } else {
        std::printf("PASS: client caption title ink is left-aligned\n");
    }

    // The call header's "amp" mark sits at y=48 in the 720px layout; the
    // caption translation must move it down to about y=84, not leave it in the
    // strip's band.
    const InkBounds header_ink =
        ink_bounds(image, pk::Rect{24.0f, 36.0f, 120.0f, 80.0f});
    if (!header_ink.found || header_ink.min_y < 60) {
        std::fprintf(stderr, "client caption did not translate the screen content\n");
        ++failures;
    } else {
        std::printf("PASS: screen content starts below the client caption\n");
    }

    const float button_y =
        (pk::CarUi::kCaptionHeight - pk::CarUi::kCaptionButtonHeight) * 0.5f;
    const float right = 1280.0f - pk::CarUi::kCaptionRightMargin;
    const pk::Rect close_rect{right - pk::CarUi::kCaptionButtonWidth, button_y,
                              pk::CarUi::kCaptionButtonWidth, pk::CarUi::kCaptionButtonHeight};
    const pk::Rect maximize_rect{close_rect.x - pk::CarUi::kCaptionButtonWidth, button_y,
                                 pk::CarUi::kCaptionButtonWidth,
                                 pk::CarUi::kCaptionButtonHeight};
    const pk::Rect minimize_rect{maximize_rect.x - pk::CarUi::kCaptionButtonWidth, button_y,
                                 pk::CarUi::kCaptionButtonWidth,
                                 pk::CarUi::kCaptionButtonHeight};
    const InkBounds close_ink = ink_bounds(image, close_rect, 90);
    const InkBounds maximize_ink = ink_bounds(image, maximize_rect, 90);
    const InkBounds minimize_ink = ink_bounds(image, minimize_rect, 90);
    const bool buttons_ink =
        close_ink.found && maximize_ink.found && minimize_ink.found &&
        close_ink.pixels >= 10 && maximize_ink.pixels >= 10 && minimize_ink.pixels >= 10;
    if (!buttons_ink) {
        std::fprintf(stderr, "client caption buttons drew no ink\n");
        ++failures;
    } else if (!(close_ink.max_x > maximize_ink.max_x &&
                 maximize_ink.max_x > minimize_ink.max_x) ||
               close_ink.max_x > 1280 - 4) {
        std::fprintf(stderr, "client caption buttons are not right-aligned\n");
        ++failures;
    } else {
        std::printf("PASS: client caption buttons are right-aligned\n");
    }
    if (buttons_ink) {
        const float close_center =
            (static_cast<float>(close_ink.min_x) + close_ink.max_x) * 0.5f;
        const float maximize_center =
            (static_cast<float>(maximize_ink.min_x) + maximize_ink.max_x) * 0.5f;
        const float minimize_center =
            (static_cast<float>(minimize_ink.min_x) + minimize_ink.max_x) * 0.5f;
        if (std::fabs(close_center - close_rect.center_x()) > 3.0f ||
            std::fabs(maximize_center - maximize_rect.center_x()) > 3.0f ||
            std::fabs(minimize_center - minimize_rect.center_x()) > 3.0f) {
            std::fprintf(stderr, "client caption icons are not centered in their buttons\n");
            ++failures;
        }
    }

    // Pairing loading state: the status line sits under the QR card and must
    // be visible while startup and the asset fetch are still running.
    ui.set_client_caption(false);
    ui.set_sheet({});
    ui.set_pairing(pk::QrCode{}, "Call Puck", "Restoring session");
    renderer.begin(display.size(), display.dpi_scale());
    ui.draw(renderer.canvas(), display.size(), 3.0f);
    renderer.end();
    display.swap();
    if (!read_ppm(output, &image)) {
        return fail("cannot read the loading frame");
    }
    const pk::Rect status = ui.status_bounds();
    if (status.w <= 0.0f || status.h <= 0.0f) {
        std::fprintf(stderr, "the pairing loading state has no status rect\n");
        ++failures;
    } else {
        const InkBounds status_ink = ink_bounds(image, status);
        if (!status_ink.found || status_ink.pixels < 20) {
            std::fprintf(stderr, "the pairing loading status drew no ink\n");
            ++failures;
        } else {
            std::printf("PASS: pairing loading status renders (%d px)\n", status_ink.pixels);
        }
    }

    // A small pairing window with a real Try Again button. The button has a
    // pill fill and label ink, and its hit rect stays inside the window.
    {
        const std::string small_output = "build/ui-retry-small.ppm";
        pk::HeadlessDisplay small(420, 640, small_output, 1);
        if (!small.initialize(&error)) {
            return fail("small headless display: " + error);
        }
        small.make_current();
        pk::Renderer small_renderer;
        if (!small_renderer.initialize("", &error)) {
            return fail("small renderer: " + error);
        }
        pk::CarUi small_ui(puck);
        small_ui.set_pairing(pk::QrCode{}, "Confirm on your phone", "", "482 913");
        small_ui.set_pairing_recovery(pk::PairingRecovery::Retry);
        small_renderer.begin(small.size(), small.dpi_scale());
        small_ui.draw(small_renderer.canvas(), small.size(), 0.0f);
        small_renderer.end();
        small.swap();
        Image retry_image;
        if (!read_ppm(small_output, &retry_image)) {
            return fail("cannot read " + small_output);
        }
        const pk::UiTarget* retry = nullptr;
        for (const pk::UiTarget& target : small_ui.hit_map().targets()) {
            if (target.action == pk::UiAction::PairingRetry) {
                retry = &target;
                break;
            }
        }
        if (!retry) {
            std::fprintf(stderr, "the small pairing screen has no Try Again target\n");
            ++failures;
        } else if (retry->rect.x < 0.0f || retry->rect.y < 0.0f ||
                   retry->rect.x + retry->rect.w > 420.0f ||
                   retry->rect.y + retry->rect.h > 640.0f) {
            std::fprintf(stderr, "Try Again leaves the small window: %.1f,%.1f %.1fx%.1f\n",
                         retry->rect.x, retry->rect.y, retry->rect.w, retry->rect.h);
            ++failures;
        } else {
            const InkBounds ink = ink_bounds(retry_image, retry->rect);
            if (!ink.found || ink.pixels < 40) {
                std::fprintf(stderr, "Try Again drew no readable ink in the small window\n");
                ++failures;
            } else {
                std::printf("PASS: small pairing Try Again renders (%d px)\n", ink.pixels);
            }
        }

        // A terminal failure with no QR or confirmation code must not paint the
        // white placeholder card; only the text and pill remain bright.
        small_ui.set_pairing(pk::QrCode{}, "Call Puck", "Pairing failed", "");
        small_ui.set_pairing_recovery(pk::PairingRecovery::Retry);
        small_renderer.begin(small.size(), small.dpi_scale());
        small_ui.draw(small_renderer.canvas(), small.size(), 0.1f);
        small_renderer.end();
        small.swap();
        Image failed_image;
        if (!read_ppm(small_output, &failed_image)) {
            return fail("cannot read the failure frame");
        }
        int white_pixels = 0;
        for (std::size_t i = 0; i + 2 < failed_image.rgb.size(); i += 3) {
            if (failed_image.rgb[i] > 240 && failed_image.rgb[i + 1] > 240 &&
                failed_image.rgb[i + 2] > 240) {
                ++white_pixels;
            }
        }
        if (white_pixels > 5000) {
            std::fprintf(stderr, "terminal pairing still painted %d white placeholder pixels\n",
                         white_pixels);
            ++failures;
        } else {
            std::printf("PASS: terminal pairing draws no white QR placeholder (%d px)\n",
                        white_pixels);
        }
    }

    // Empty output path: the headless display must not try to write a PPM.
    const auto previous_directory = std::filesystem::current_path();
    const auto scratch = previous_directory / "build" / "empty-output";
    std::error_code scratch_error;
    std::filesystem::remove_all(scratch, scratch_error);
    std::filesystem::create_directories(scratch, scratch_error);
    std::filesystem::current_path(scratch);
    {
        pk::HeadlessDisplay quiet(64, 64, "", 1);
        if (!quiet.initialize(&error)) {
            std::filesystem::current_path(previous_directory);
            return fail("empty-output headless display: " + error);
        }
        quiet.swap();
    }
    std::filesystem::current_path(previous_directory);
    const bool empty_output = std::filesystem::is_empty(scratch, scratch_error);
    std::filesystem::remove_all(scratch, scratch_error);
    if (!empty_output) {
        std::fprintf(stderr, "an empty output path wrote a file\n");
        ++failures;
    } else {
        std::printf("PASS: an empty output path writes no PPM\n");
    }

    if (failures == 0) {
        std::printf("PASS: button label is vertically and horizontally centered\n");
    }
    return failures == 0 ? 0 : 1;
}
