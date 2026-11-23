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

InkBounds ink_bounds(const Image& image, const pk::Rect& rect) {
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
            const int luma = (red * 299 + green * 587 + blue * 114) / 1000;
            if (luma < 140) {
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
    if (failures == 0) {
        std::printf("PASS: button label is vertically and horizontally centered\n");
    }
    return failures == 0 ? 0 : 1;
}
