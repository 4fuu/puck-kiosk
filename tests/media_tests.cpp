#include "app/media_overlay.h"
#include "platform/http.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

const unsigned char kTinyPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x01, 0x03, 0x00, 0x00, 0x00, 0x48, 0x78, 0x9f, 0x67, 0x00, 0x00, 0x00,
    0x03, 0x50, 0x4c, 0x54, 0x45, 0xc0, 0x40, 0x30, 0xe0, 0x75, 0xd4, 0x33,
    0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63, 0x60,
    0x60, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x27, 0x34, 0x27, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

class DummyCanvas final : public pk::Canvas {
public:
    bool initialize(const std::string&, std::string*) override { return true; }
    void begin_frame(pk::Size, float) override {}
    void end_frame() override {}

    pk::ImageHandle create_image(const std::uint8_t* rgba, int width, int height) override {
        if (!rgba || width <= 0 || height <= 0) {
            return pk::kInvalidImage;
        }
        ++created;
        return static_cast<pk::ImageHandle>(created);
    }
    void draw_image(pk::ImageHandle, pk::Rect, float) override {}
    void release_image(pk::ImageHandle) override { ++released; }

    void clear(pk::Color) override {}
    void fill_rect(pk::Rect, pk::Color) override {}
    void fill_rounded_rect(pk::Rect, float, pk::Color) override {}
    void fill_circle(float, float, float, pk::Color) override {}
    void fill_ellipse(float, float, float, float, pk::Color) override {}
    void fill_triangle(float, float, float, float, float, float, pk::Color) override {}
    void stroke_circle(float, float, float, float, pk::Color) override {}
    void stroke_line(float, float, float, float, float, pk::Color) override {}
    void text(float, float, float, pk::Color, pk::TextAlign, const std::string&) override {}
    float measure_text(float size, const std::string& value) override {
        return static_cast<float>(value.size()) * size * 0.5f;
    }

    int created = 0;
    int released = 0;
};

class DummyHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& request, std::string* error) override {
        urls.push_back(request.url);
        pk::HttpResponse response;
        if (fail) {
            response.status = 404;
            return response;
        }
        response.status = 200;
        response.body.assign(reinterpret_cast<const char*>(kTinyPng),
                             reinterpret_cast<const char*>(kTinyPng) + sizeof(kTinyPng));
        (void)error;
        return response;
    }

    bool fail = false;
    std::vector<std::string> urls;
};

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) {
        std::printf("ok   %s\n", label);
        return;
    }
    std::printf("FAIL %s\n", label);
    ++failures;
}

void pump(pk::MediaOverlay& overlay, int milliseconds = 1500) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!overlay.settled() && std::chrono::steady_clock::now() < deadline) {
        overlay.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    overlay.poll();
}

nlohmann::json image_item(const std::string& id, const std::string& url,
                          const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json item = {{"id", id}, {"kind", "image"}, {"url", url}};
    for (auto entry = extra.begin(); entry != extra.end(); ++entry) {
        item[entry.key()] = entry.value();
    }
    return item;
}

void test_wire_shape_and_rewrite() {
    DummyCanvas canvas;
    DummyHttp http;
    pk::MediaOverlay overlay(&http, "https://ampcode.com", canvas);

    const nlohmann::json presentation = {
        {"id", "p1"},
        {"title", "Generated picture"},
        {"items",
         nlohmann::json::array({
             image_item("a", "https://static.example.com/generated/one.png?token=abc&x=1#frag"),
             {{"id", "video"}, {"kind", "video"}, {"url", "https://static.example.com/v.mp4"}},
             {{"id", "c"}, {"url", "https://static.example.com/generated/three.png"}},
         })},
    };
    std::string error;
    check(overlay.set_presentation(presentation, false, &error), "accepts the actor presentation");
    check(overlay.presentation() && overlay.presentation()->items.size() == 2,
          "skips video items and keeps images");
    pump(overlay);
    check(overlay.settled(), "settles after decoding");
    check(overlay.current_error().empty(), "decoded item has no error");
    check(!http.urls.empty(), "fetches the current image");
    if (!http.urls.empty()) {
        check(http.urls[0] == "https://ampcode.com/generated/one.png?token=abc&x=1",
              "rewrites to the app origin and keeps path/query");
    }
    check(overlay.presentation()->title == "Generated picture", "keeps the presentation title");
}

void test_expiry_does_not_fetch() {
    DummyCanvas canvas;
    DummyHttp http;
    pk::MediaOverlay overlay(&http, "https://ampcode.com", canvas);

    const nlohmann::json presentation = {
        {"id", "p2"},
        {"items",
         nlohmann::json::array({
             image_item("old", "https://static.example.com/old.png",
                        {{"expiresAt", "2000-01-01T00:00:00.000Z"}}),
         })},
    };
    std::string error;
    check(overlay.set_presentation(presentation, false, &error), "accepts an expired item");
    pump(overlay, 100);
    check(http.urls.empty(), "does not fetch an expired item");
    check(overlay.settled(), "expired item is settled");
    check(overlay.current_error() == "Image expired", "shows the expired error");
}

void test_fetch_failure() {
    DummyCanvas canvas;
    DummyHttp http;
    http.fail = true;
    pk::MediaOverlay overlay(&http, "https://ampcode.com", canvas);

    const nlohmann::json presentation = {
        {"id", "p3"},
        {"items", nlohmann::json::array({image_item("missing", "https://static.example.com/x.png")})},
    };
    std::string error;
    check(overlay.set_presentation(presentation, false, &error), "accepts a failing item");
    pump(overlay);
    check(overlay.settled(), "failed item is settled");
    check(overlay.current_error().find("404") != std::string::npos, "reports the HTTP status");
}

void test_validation_and_close() {
    DummyCanvas canvas;
    DummyHttp http;
    pk::MediaOverlay overlay(&http, "https://ampcode.com", canvas);

    const nlohmann::json bad_url = {
        {"id", "p4"},
        {"items", nlohmann::json::array({image_item("bad", "ftp://example.com/a.png")})},
    };
    std::string error;
    check(!overlay.set_presentation(bad_url, false, &error), "rejects non-http actor URLs");

    const nlohmann::json no_items = {{"id", "p5"}, {"items", nlohmann::json::array()}};
    check(!overlay.set_presentation(no_items, false, &error), "rejects an empty item list");

    const nlohmann::json bad_kind = {
        {"id", "p5"},
        {"items", nlohmann::json::array({{{"id", "audio"}, {"kind", "audio"},
                                          {"url", "https://static.example.com/a.mp3"}}})},
    };
    check(!overlay.set_presentation(bad_kind, false, &error), "rejects unsupported item kinds");

    const nlohmann::json null_presentation = nullptr;
    check(overlay.set_presentation(null_presentation, false, &error), "accepts a null presentation");
    check(!overlay.visible(), "null closes the overlay");
}

void test_cache_bound_and_release() {
    DummyCanvas canvas;
    DummyHttp http;
    pk::MediaOverlay overlay(&http, "https://ampcode.com", canvas);

    nlohmann::json items = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) {
        items.push_back(image_item("item" + std::to_string(i),
                                   "https://static.example.com/" + std::to_string(i) + ".png"));
    }
    const nlohmann::json presentation = {{"id", "p6"}, {"items", items}};
    std::string error;
    check(overlay.set_presentation(presentation, false, &error), "accepts four items");

    for (int i = 0; i < 4; ++i) {
        overlay.set_index(i);
        pump(overlay);
        const std::string label = "item " + std::to_string(i) + " decodes";
        check(overlay.current_error().empty(), label.c_str());
    }
    check(canvas.created == 4, "decodes each item once");
    check(canvas.created - canvas.released <= 3, "keeps at most three textures");

    overlay.close();
    check(canvas.released == canvas.created, "releases every texture on close");
    check(!overlay.visible(), "closed overlay is hidden");
}

}  // namespace

int main() {
    test_wire_shape_and_rewrite();
    test_expiry_does_not_fetch();
    test_fetch_failure();
    test_validation_and_close();
    test_cache_bound_and_release();

    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
