#include "app/assets.h"
#include "app/media_overlay.h"
#include "core/sha256.h"
#include "platform/http.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
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
    void text(float, float, const pk::TextStyle&, const std::string&) override {}
    float measure_text(const pk::TextStyle& style, const std::string& value) override {
        return static_cast<float>(value.size()) * style.size * 0.5f;
    }
    void fill_path(const std::string&, pk::Rect, pk::Rect, pk::Color) override {}

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

const unsigned char kGzipAssetOne[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x18, 0x55, 0x05, 0x6b, 0x02, 0xff, 0x4b, 0x2c, 0x2e, 0x4e,
    0x2d, 0xd1, 0xcd, 0xcf, 0x4b, 0x05, 0x00, 0x6a, 0x2d, 0x50, 0xd1, 0x09, 0x00, 0x00,
    0x00,
};

class AssetHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& request, std::string* error) override {
        urls.push_back(request.url);
        pk::HttpResponse response;
        const auto found = bodies.find(request.url);
        if (found == bodies.end()) {
            response.status = 404;
            return response;
        }
        response.status = status;
        response.body = found->second;
        if (!content_encoding.empty()) {
            response.headers.push_back({"Content-Encoding", content_encoding});
        }
        (void)error;
        return response;
    }

    std::map<std::string, std::string> bodies;
    std::vector<std::string> urls;
    std::string content_encoding;
    int status = 200;
};

std::filesystem::path scratch_directory(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / (name + "-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    return path;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

nlohmann::json asset_entry(const std::string& path, const std::string& url,
                           const std::string& body) {
    return nlohmann::json{
        {"path", path},
        {"url", url},
        {"sha256", pk::sha256_hex(body)},
        {"bytes", static_cast<int>(body.size())},
    };
}

void test_sha256_vectors() {
    check(pk::sha256_hex(std::string{}) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 of the empty string");
    check(pk::sha256_hex(std::string("abc")) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of abc");
    check(pk::sha256_hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 of the two-block vector");
    check(pk::sha256_hex(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256 of one million a");
}

void test_asset_fetch() {
    const auto root = scratch_directory("puck-kiosk-assets-test");
    const std::string one = "asset-one";
    const std::string two = "asset-two";
    AssetHttp http;
    http.bodies["https://assets.test/one"] = one;
    http.bodies["https://assets.test/two"] = two;
    const nlohmann::json manifest = {
        {"assets", nlohmann::json::array({
                       asset_entry("one.bin", "https://assets.test/one", one),
                       asset_entry("sub/two.bin", "https://assets.test/two", two),
                   })},
    };
    write_text(root / "manifest.json", manifest.dump());

    const auto cache = root / "cache";
    const auto manifest_path = root / "manifest.json";
    {
        pk::AssetFetcher fetcher(http, cache.string(), manifest_path.string());
        fetcher.start();
        fetcher.wait();
        const pk::AssetFetchStatus status = fetcher.status();
        check(status.finished && status.ok, "asset fetch verifies every file");
        check(status.completed == 2 && status.total == 2, "asset fetch reports progress");
        check(read_text(cache / "one.bin") == one, "asset fetch writes the first file");
        check(read_text(cache / "sub/two.bin") == two, "asset fetch creates subdirectories");
        check(http.urls.size() == 2, "asset fetch downloads each missing file once");
    }
    {
        AssetHttp cached_http;
        pk::AssetFetcher fetcher(cached_http, cache.string(), manifest_path.string());
        fetcher.start();
        fetcher.wait();
        check(fetcher.status().ok, "a warm cache verifies without downloading");
        check(cached_http.urls.empty(), "a warm cache makes no HTTP requests");
    }
    {
        write_text(cache / "one.bin", "bad");
        const std::size_t before = http.urls.size();
        pk::AssetFetcher fetcher(http, cache.string(), manifest_path.string());
        fetcher.start();
        fetcher.wait();
        check(fetcher.status().ok, "a tampered cache file is replaced");
        check(http.urls.size() == before + 1, "only the tampered file is downloaded");
        check(read_text(cache / "one.bin") == one, "the replaced file matches the manifest");
    }

    {
        const auto gzip_root = scratch_directory("puck-kiosk-assets-gzip");
        AssetHttp gzip_http;
        gzip_http.bodies["https://assets.test/one"] =
            std::string(reinterpret_cast<const char*>(kGzipAssetOne), sizeof(kGzipAssetOne));
        gzip_http.content_encoding = "gzip";
        const nlohmann::json gzip_manifest = {
            {"assets", nlohmann::json::array({asset_entry("one.bin", "https://assets.test/one", one)})},
        };
        write_text(gzip_root / "manifest.json", gzip_manifest.dump());
        pk::AssetFetcher fetcher(gzip_http, (gzip_root / "cache").string(),
                                 (gzip_root / "manifest.json").string());
        fetcher.start();
        fetcher.wait();
        check(fetcher.status().ok, "a gzip-encoded asset is decoded and verified");
        check(read_text(gzip_root / "cache" / "one.bin") == one,
              "the decoded gzip asset is written");
        std::error_code cleanup_error;
        std::filesystem::remove_all(gzip_root, cleanup_error);
    }

    const auto failure_root = scratch_directory("puck-kiosk-assets-failure");
    {
        AssetHttp bad_http;
        bad_http.bodies["https://assets.test/one"] = "wrong";
        const nlohmann::json bad_manifest = {
            {"assets", nlohmann::json::array({asset_entry("one.bin", "https://assets.test/one", one)})},
        };
        write_text(failure_root / "manifest.json", bad_manifest.dump());
        pk::AssetFetcher fetcher(bad_http, (failure_root / "cache").string(),
                                 (failure_root / "manifest.json").string());
        fetcher.start();
        fetcher.wait();
        const pk::AssetFetchStatus status = fetcher.status();
        check(status.finished && !status.ok, "a size mismatch fails the asset fetch");
        check(!status.error.empty(), "the failed fetch reports an error");
        check(!std::filesystem::exists(failure_root / "cache" / "one.bin"),
              "a failed download leaves no cache file");
    }
    {
        AssetHttp unused_http;
        const nlohmann::json bad_manifest = {
            {"assets", nlohmann::json::array(
                           {asset_entry("../escape.bin", "https://assets.test/one", one)})},
        };
        write_text(failure_root / "manifest.json", bad_manifest.dump());
        pk::AssetFetcher fetcher(unused_http, (failure_root / "cache").string(),
                                 (failure_root / "manifest.json").string());
        fetcher.start();
        fetcher.wait();
        check(!fetcher.status().ok, "an unsafe manifest path fails the asset fetch");
        check(unused_http.urls.empty(), "an unsafe path is rejected before downloading");
        check(!std::filesystem::exists(failure_root / "escape.bin"),
              "an unsafe path cannot escape the cache root");
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove_all(failure_root, cleanup_error);
}

}  // namespace

int main() {
    test_sha256_vectors();
    test_asset_fetch();
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
