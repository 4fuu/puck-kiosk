#include "app/media_overlay.h"

#include "core/log.h"
#include "platform/http.h"

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <set>
#include <utility>

namespace pk {
namespace {

constexpr int kMaxDimension = 4096;
constexpr int kMaxPixels = 16 * 1024 * 1024;
constexpr int kTargetDimension = 1280;
constexpr std::int64_t kMaxImageBytes = 24 * 1024 * 1024;
constexpr std::size_t kMaxCachedImages = 3;
constexpr std::size_t kMaxQueuedResults = 2;

const char* const kNoImageError = "Could not load the picture";
const char* const kExpiredError = "Image expired";

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string truncate(std::string value, std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }
    std::size_t cut = limit;
    // Do not split a UTF-8 code point at the cut.
    if (cut < value.size() && (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80) {
        while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80) {
            --cut;
        }
        if (cut > 0) {
            --cut;
        }
    }
    value.resize(cut);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool is_http_url(const std::string& url) {
    auto has_prefix = [&url](const char* prefix) {
        const std::size_t length = std::strlen(prefix);
        if (url.size() < length) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) {
                return false;
            }
        }
        return true;
    };
    return has_prefix("http://") || has_prefix("https://");
}

std::string strip_trailing_slash(std::string value) {
    while (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

// The car page turns a presentation URL into a same-origin path so the
// session cookie and any server-side proxy apply:
//   `${location.origin}${new URL(url).pathname}${search}`
std::string rewrite_to_origin(const std::string& url, const std::string& origin) {
    if (origin.empty()) {
        return url;
    }
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return url;
    }
    const std::size_t authority_end = scheme_end + 3;
    const std::size_t path_start = url.find('/', authority_end);
    const std::size_t query_start = url.find('?', authority_end);
    std::string path;
    if (path_start == std::string::npos) {
        path = "/";
        if (query_start != std::string::npos) {
            path += url.substr(query_start);
        }
    } else {
        path = url.substr(path_start);
    }
    const std::size_t fragment = path.find('#');
    if (fragment != std::string::npos) {
        path.erase(fragment);
    }
    if (path.empty()) {
        path = "/";
    }
    return strip_trailing_slash(origin) + path;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// expiresAt is a non-empty string in the actor schema. Accept ISO-8601 with an
// optional fractional second and zone, plus bare epoch seconds/milliseconds.
std::int64_t parse_expiry_ms(const std::string& raw) {
    const std::string value = trim(raw);
    if (value.empty()) {
        return 0;
    }
    bool numeric = true;
    for (const char character : value) {
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        try {
            const long long parsed = std::stoll(value);
            if (parsed <= 0) {
                return 0;
            }
            return parsed > 100000000000LL ? static_cast<std::int64_t>(parsed)
                                           : static_cast<std::int64_t>(parsed) * 1000;
        } catch (...) {
            return 0;
        }
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) != 6) {
        return 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 60) {
        return 0;
    }

    std::size_t position = value.find('T');
    if (position == std::string::npos || position + 9 > value.size()) {
        return 0;
    }
    position += 9;  // "T" plus "HH:MM:SS"
    int milliseconds = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        int digits = 0;
        while (position < value.size() && std::isdigit(static_cast<unsigned char>(value[position])) &&
               digits < 3) {
            milliseconds = milliseconds * 10 + (value[position] - '0');
            ++position;
            ++digits;
        }
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
        while (position < value.size() &&
               std::isdigit(static_cast<unsigned char>(value[position]))) {
            ++position;
        }
    }

    int offset_seconds = 0;
    if (position < value.size() && (value[position] == 'Z' || value[position] == 'z')) {
        ++position;
    } else if (position < value.size() &&
               (value[position] == '+' || value[position] == '-')) {
        const int sign = value[position] == '-' ? -1 : 1;
        int offset_hour = 0;
        int offset_minute = 0;
        if (std::sscanf(value.c_str() + position + 1, "%2d:%2d", &offset_hour, &offset_minute) !=
            2) {
            return 0;
        }
        offset_seconds = sign * (offset_hour * 3600 + offset_minute * 60);
    }

    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    utc.tm_isdst = 0;
    const std::time_t seconds = timegm(&utc);
    if (seconds == static_cast<std::time_t>(-1)) {
        return 0;
    }
    return (static_cast<std::int64_t>(seconds) - offset_seconds) * 1000 + milliseconds;
}

bool valid_dimensions(int width, int height) {
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        return false;
    }
    return static_cast<std::int64_t>(width) * height <= kMaxPixels;
}

void fit_dimensions(int width, int height, int max_side, int* target_width, int* target_height) {
    if (width <= max_side && height <= max_side) {
        *target_width = width;
        *target_height = height;
        return;
    }
    if (width >= height) {
        *target_width = max_side;
        *target_height = std::max(1, static_cast<int>(std::lround(
                                         static_cast<double>(height) * max_side / width)));
    } else {
        *target_height = max_side;
        *target_width = std::max(1, static_cast<int>(std::lround(
                                        static_cast<double>(width) * max_side / height)));
    }
}

std::vector<std::uint8_t> resize_rgba(const std::uint8_t* source, int source_width,
                                      int source_height, int target_width, int target_height) {
    std::vector<std::uint8_t> target(static_cast<std::size_t>(target_width) * target_height * 4);
    const float x_scale = static_cast<float>(source_width) / target_width;
    const float y_scale = static_cast<float>(source_height) / target_height;
    for (int y = 0; y < target_height; ++y) {
        const float source_y = (y + 0.5f) * y_scale - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0, source_height - 1);
        const int y1 = std::clamp(y0 + 1, 0, source_height - 1);
        const float fy = std::clamp(source_y - std::floor(source_y), 0.0f, 1.0f);
        for (int x = 0; x < target_width; ++x) {
            const float source_x = (x + 0.5f) * x_scale - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, source_width - 1);
            const int x1 = std::clamp(x0 + 1, 0, source_width - 1);
            const float fx = std::clamp(source_x - std::floor(source_x), 0.0f, 1.0f);
            const std::uint8_t* p00 =
                source + (static_cast<std::size_t>(y0) * source_width + x0) * 4;
            const std::uint8_t* p10 =
                source + (static_cast<std::size_t>(y0) * source_width + x1) * 4;
            const std::uint8_t* p01 =
                source + (static_cast<std::size_t>(y1) * source_width + x0) * 4;
            const std::uint8_t* p11 =
                source + (static_cast<std::size_t>(y1) * source_width + x1) * 4;
            std::uint8_t* out =
                target.data() + (static_cast<std::size_t>(y) * target_width + x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                const float top = p00[channel] + (p10[channel] - p00[channel]) * fx;
                const float bottom = p01[channel] + (p11[channel] - p01[channel]) * fx;
                out[channel] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(top + (bottom - top) * fy), 0L, 255L));
            }
        }
    }
    return target;
}

bool decode_webp(const std::uint8_t* data, std::size_t size, int* width, int* height,
                 std::vector<std::uint8_t>* rgba, std::string* error) {
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK) {
        *error = "invalid WebP data";
        return false;
    }
    if (!valid_dimensions(features.width, features.height)) {
        *error = "WebP dimensions are not supported";
        return false;
    }
    int target_width = features.width;
    int target_height = features.height;
    fit_dimensions(features.width, features.height, kTargetDimension, &target_width,
                   &target_height);

    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
        *error = "cannot initialize the WebP decoder";
        return false;
    }
    config.output.colorspace = MODE_RGBA;
    if (target_width != features.width || target_height != features.height) {
        config.options.use_scaling = 1;
        config.options.scaled_width = target_width;
        config.options.scaled_height = target_height;
    }
    const VP8StatusCode status = WebPDecode(data, size, &config);
    if (status != VP8_STATUS_OK) {
        WebPFreeDecBuffer(&config.output);
        *error = "cannot decode WebP";
        return false;
    }
    *width = config.output.width;
    *height = config.output.height;
    if (!valid_dimensions(*width, *height)) {
        WebPFreeDecBuffer(&config.output);
        *error = "WebP decoded dimensions are not supported";
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(*width) * *height * 4;
    rgba->assign(config.output.u.RGBA.rgba, config.output.u.RGBA.rgba + bytes);
    WebPFreeDecBuffer(&config.output);
    return true;
}

bool decode_png(const std::uint8_t* data, std::size_t size, int* width, int* height,
                std::vector<std::uint8_t>* rgba, std::string* error) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, size)) {
        *error = "invalid PNG data";
        return false;
    }
    if (!valid_dimensions(static_cast<int>(image.width), static_cast<int>(image.height))) {
        png_image_free(&image);
        *error = "PNG dimensions are not supported";
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        *error = "cannot decode PNG";
        return false;
    }
    *width = static_cast<int>(image.width);
    *height = static_cast<int>(image.height);
    png_image_free(&image);

    int target_width = *width;
    int target_height = *height;
    fit_dimensions(*width, *height, kTargetDimension, &target_width, &target_height);
    if (target_width == *width && target_height == *height) {
        *rgba = std::move(pixels);
    } else {
        *rgba = resize_rgba(pixels.data(), *width, *height, target_width, target_height);
        *width = target_width;
        *height = target_height;
    }
    return true;
}

struct JpegError {
    jpeg_error_mgr manager;
    std::jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegError*>(info->err);
    std::longjmp(error->jump, 1);
}

void jpeg_ignore_message(j_common_ptr) {}

bool decode_jpeg(const std::uint8_t* data, std::size_t size, int* width, int* height,
                 std::vector<std::uint8_t>* rgba, std::string* error) {
    jpeg_decompress_struct info{};
    JpegError jpeg_error{};
    info.err = jpeg_std_error(&jpeg_error.manager);
    jpeg_error.manager.error_exit = jpeg_error_exit;
    jpeg_error.manager.output_message = jpeg_ignore_message;

    unsigned char* volatile rgb = nullptr;
    volatile bool created = false;
    if (setjmp(jpeg_error.jump) != 0) {
        std::free(rgb);
        if (created) {
            jpeg_destroy_decompress(&info);
        }
        *error = "cannot decode JPEG";
        return false;
    }

    jpeg_create_decompress(&info);
    created = true;
    jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
    jpeg_read_header(&info, TRUE);
    if (!valid_dimensions(static_cast<int>(info.image_width),
                          static_cast<int>(info.image_height))) {
        jpeg_destroy_decompress(&info);
        *error = "JPEG dimensions are not supported";
        return false;
    }
    if (info.image_width > static_cast<unsigned>(kTargetDimension) * 2 &&
        info.image_height > static_cast<unsigned>(kTargetDimension) * 2) {
        info.scale_num = 1;
        info.scale_denom = 2;
    }
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    const int source_width = static_cast<int>(info.output_width);
    const int source_height = static_cast<int>(info.output_height);
    if (!valid_dimensions(source_width, source_height)) {
        jpeg_destroy_decompress(&info);
        *error = "JPEG decoded dimensions are not supported";
        return false;
    }
    rgb = static_cast<unsigned char*>(
        std::malloc(static_cast<std::size_t>(source_width) * source_height * 3));
    if (!rgb) {
        jpeg_destroy_decompress(&info);
        *error = "out of memory for the JPEG";
        return false;
    }
    while (info.output_scanline < info.output_height) {
        JSAMPROW row = rgb + static_cast<std::size_t>(info.output_scanline) * source_width * 3;
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    created = false;

    std::vector<std::uint8_t> full(static_cast<std::size_t>(source_width) * source_height * 4);
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(source_width) * source_height;
         ++pixel) {
        full[pixel * 4 + 0] = rgb[pixel * 3 + 0];
        full[pixel * 4 + 1] = rgb[pixel * 3 + 1];
        full[pixel * 4 + 2] = rgb[pixel * 3 + 2];
        full[pixel * 4 + 3] = 255;
    }
    std::free(rgb);
    rgb = nullptr;

    int target_width = source_width;
    int target_height = source_height;
    fit_dimensions(source_width, source_height, kTargetDimension, &target_width, &target_height);
    if (target_width == source_width && target_height == source_height) {
        *rgba = std::move(full);
    } else {
        *rgba = resize_rgba(full.data(), source_width, source_height, target_width,
                            target_height);
    }
    *width = target_width;
    *height = target_height;
    return true;
}

bool decode_image(const std::uint8_t* bytes, std::size_t size, int* width, int* height,
                  std::vector<std::uint8_t>* rgba, std::string* error) {
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 &&
        std::memcmp(bytes + 8, "WEBP", 4) == 0) {
        return decode_webp(bytes, size, width, height, rgba, error);
    }
    static const std::uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (size >= sizeof(kPngSignature) &&
        std::memcmp(bytes, kPngSignature, sizeof(kPngSignature)) == 0) {
        return decode_png(bytes, size, width, height, rgba, error);
    }
    if (size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
        return decode_jpeg(bytes, size, width, height, rgba, error);
    }
    *error = "unsupported image format";
    return false;
}

bool read_file(const std::string& path, std::int64_t max_bytes, std::vector<std::uint8_t>* data,
               std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "cannot open the picture file";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) {
        *error = "cannot read the picture file";
        return false;
    }
    if (length > max_bytes) {
        *error = "the picture is too large";
        return false;
    }
    input.seekg(0, std::ios::beg);
    data->resize(static_cast<std::size_t>(length));
    if (length > 0 && !input.read(reinterpret_cast<char*>(data->data()), length)) {
        *error = "cannot read the picture file";
        return false;
    }
    return true;
}

std::string header_value(const HttpResponse& response, const char* name) {
    for (const HttpHeader& header : response.headers) {
        if (header.name.size() == std::strlen(name)) {
            bool equal = true;
            for (std::size_t i = 0; i < header.name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(header.name[i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                return header.value;
            }
        }
    }
    return {};
}

std::string fit_text(Canvas& canvas, float size, const std::string& text, float max_width) {
    if (max_width <= 0.0f || text.empty() ||
        canvas.measure_text(size, text) <= max_width) {
        return text;
    }
    std::string shortened = text;
    while (!shortened.empty()) {
        shortened.pop_back();
        while (!shortened.empty() &&
               (static_cast<unsigned char>(shortened.back()) & 0xc0) == 0x80) {
            shortened.pop_back();
        }
        const std::string candidate = shortened + "...";
        if (canvas.measure_text(size, candidate) <= max_width) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::string> wrap_text(Canvas& canvas, float size, const std::string& text,
                                   float max_width, int max_lines) {
    std::vector<std::string> lines;
    if (text.empty() || max_lines <= 0) {
        return lines;
    }
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
                return lines;
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
    return lines;
}

Rect fit_rect(const Rect& bounds, int width, int height) {
    if (width <= 0 || height <= 0 || bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return Rect{};
    }
    const float scale = std::min(bounds.w / width, bounds.h / height);
    const float draw_width = width * scale;
    const float draw_height = height * scale;
    return Rect{bounds.center_x() - draw_width * 0.5f, bounds.center_y() - draw_height * 0.5f,
                draw_width, draw_height};
}

void draw_chevron(Canvas& canvas, float center_x, float center_y, float size, bool point_left,
                  Color color) {
    const float direction = point_left ? -1.0f : 1.0f;
    const float x_tip = center_x + direction * size * 0.30f;
    const float x_back = center_x - direction * size * 0.22f;
    canvas.stroke_line(x_back, center_y - size * 0.36f, x_tip, center_y, 5.0f, color);
    canvas.stroke_line(x_tip, center_y, x_back, center_y + size * 0.36f, 5.0f, color);
}

}  // namespace

MediaOverlay::MediaOverlay(HttpClient* http, std::string app_origin, Canvas& canvas)
    : http_(http), app_origin_(std::move(app_origin)), canvas_(canvas) {
    worker_ = std::thread(&MediaOverlay::worker_main, this);
}

MediaOverlay::~MediaOverlay() {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        stopping_ = true;
        requests_.clear();
    }
    worker_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    clear_images();
}

void MediaOverlay::on_actor_message(const std::string& text) {
    const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded()) {
        return;
    }

    auto handle = [this](const nlohmann::json& event) {
        if (!event.is_object()) {
            return;
        }
        std::string type;
        std::string method;
        if (event.contains("type") && event["type"].is_string()) {
            type = event["type"].get<std::string>();
        }
        if (event.contains("method") && event["method"].is_string()) {
            method = event["method"].get<std::string>();
        }
        const bool named_event =
            event.contains("event") && event["event"].is_string() &&
            event["event"].get<std::string>() == "puck_media_presentation";
        if (type != "puck_media_presentation" && method != "puck_media_presentation" &&
            !named_event) {
            return;
        }

        const nlohmann::json* payload = &event;
        if (event.contains("params") && event["params"].is_object()) {
            payload = &event["params"];
        } else if (event.contains("data") && event["data"].is_object()) {
            payload = &event["data"];
        } else if (event.contains("event") && event["event"].is_object()) {
            payload = &event["event"];
        }
        const nlohmann::json* presentation = nullptr;
        if (event.contains("presentation")) {
            presentation = &event["presentation"];
        } else if (payload->contains("presentation")) {
            presentation = &(*payload)["presentation"];
        }
        if (!presentation) {
            return;
        }
        std::string error;
        if (!set_presentation(*presentation, false, &error)) {
            log::warn("media: ignoring presentation: %s", error.c_str());
        }
    };

    if (message.is_array()) {
        for (const nlohmann::json& event : message) {
            handle(event);
        }
        return;
    }
    handle(message);
}

bool MediaOverlay::set_presentation(const nlohmann::json& presentation, bool allow_local_paths,
                                    std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (presentation.is_null()) {
        close();
        return true;
    }
    if (!presentation.is_object()) {
        return fail("presentation is not an object");
    }

    MediaPresentation parsed;
    if (!presentation.contains("id") || !presentation["id"].is_string()) {
        return fail("presentation id is missing");
    }
    parsed.id = trim(presentation["id"].get<std::string>());
    if (parsed.id.empty() || parsed.id.size() > 64) {
        return fail("presentation id is invalid");
    }
    if (presentation.contains("title") && !presentation["title"].is_null()) {
        if (!presentation["title"].is_string()) {
            return fail("presentation title is not a string");
        }
        parsed.title = truncate(trim(presentation["title"].get<std::string>()), 80);
    }
    if (!presentation.contains("items") || !presentation["items"].is_array()) {
        return fail("presentation items are missing");
    }
    const nlohmann::json& items = presentation["items"];
    if (items.empty() || items.size() > 8) {
        return fail("presentation must have 1 to 8 items");
    }

    std::set<std::string> ids;
    for (const nlohmann::json& entry : items) {
        if (!entry.is_object()) {
            return fail("presentation item is not an object");
        }
        MediaItem item;
        if (!entry.contains("id") || !entry["id"].is_string()) {
            return fail("presentation item id is missing");
        }
        item.id = trim(entry["id"].get<std::string>());
        if (item.id.empty() || item.id.size() > 64 || !ids.insert(item.id).second) {
            return fail("presentation item id is invalid or duplicated");
        }
        if (!entry.contains("url") || !entry["url"].is_string()) {
            return fail("presentation item url is missing");
        }
        const std::string raw_url = trim(entry["url"].get<std::string>());
        if (raw_url.empty()) {
            return fail("presentation item url is empty");
        }

        // The web schema requires kind; older senders omit it. Video is out of
        // scope for this milestone, so skip those items instead of failing.
        if (entry.contains("kind") && !entry["kind"].is_null()) {
            if (!entry["kind"].is_string()) {
                return fail("presentation item kind is not a string");
            }
            const std::string kind = entry["kind"].get<std::string>();
            if (kind == "video") {
                continue;
            }
            if (kind != "image") {
                return fail("presentation item kind is not supported");
            }
        }

        if (is_http_url(raw_url)) {
            item.url = rewrite_to_origin(raw_url, app_origin_);
        } else if (allow_local_paths) {
            item.url = raw_url;
        } else {
            return fail("presentation item url is not http or https");
        }

        if (entry.contains("mimeType") && !entry["mimeType"].is_null()) {
            if (!entry["mimeType"].is_string()) {
                return fail("presentation item mimeType is not a string");
            }
            item.mime_type = truncate(trim(entry["mimeType"].get<std::string>()), 100);
        }
        if (entry.contains("caption") && !entry["caption"].is_null()) {
            if (!entry["caption"].is_string()) {
                return fail("presentation item caption is not a string");
            }
            item.caption = truncate(trim(entry["caption"].get<std::string>()), 200);
        }
        if (entry.contains("expiresAt") && !entry["expiresAt"].is_null()) {
            if (!entry["expiresAt"].is_string()) {
                return fail("presentation item expiresAt is not a string");
            }
            item.expires_at = trim(entry["expiresAt"].get<std::string>());
            item.expires_at_ms = parse_expiry_ms(item.expires_at);
            if (item.expires_at_ms == 0 && !item.expires_at.empty()) {
                log::warn("media: item %s has an unparseable expiresAt", item.id.c_str());
            }
        }
        if (entry.contains("sizeBytes") && !entry["sizeBytes"].is_null()) {
            if (!entry["sizeBytes"].is_number_integer() && !entry["sizeBytes"].is_number_unsigned()) {
                return fail("presentation item sizeBytes is not an integer");
            }
            if (entry["sizeBytes"].is_number_unsigned()) {
                const std::uint64_t size = entry["sizeBytes"].get<std::uint64_t>();
                if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    return fail("presentation item sizeBytes is too large");
                }
                item.size_bytes = static_cast<std::int64_t>(size);
            } else {
                const std::int64_t size = entry["sizeBytes"].get<std::int64_t>();
                if (size < 0) {
                    return fail("presentation item sizeBytes is negative");
                }
                item.size_bytes = size;
            }
        }
        parsed.items.push_back(std::move(item));
    }

    if (parsed.items.empty()) {
        return fail("presentation has no image items");
    }

    ++generation_;
    clear_work();
    clear_images();
    statuses_.assign(parsed.items.size(), ItemStatus{});
    presentation_ = std::move(parsed);
    index_ = 0;
    log_state("media: presentation");
    return true;
}

void MediaOverlay::close() {
    const bool was_open = presentation_.has_value();
    ++generation_;
    presentation_.reset();
    index_ = 0;
    statuses_.clear();
    clear_work();
    clear_images();
    if (was_open) {
        log::info("media: overlay closed");
    }
}

void MediaOverlay::next() {
    if (!presentation_) {
        return;
    }
    const int last = static_cast<int>(presentation_->items.size()) - 1;
    if (index_ < last) {
        ++index_;
    }
}

void MediaOverlay::previous() {
    if (index_ > 0) {
        --index_;
    }
}

void MediaOverlay::set_index(int index) {
    if (!presentation_ || presentation_->items.empty()) {
        return;
    }
    index_ = std::clamp(index, 0, static_cast<int>(presentation_->items.size()) - 1);
}

bool MediaOverlay::visible() const {
    return presentation_.has_value() && !presentation_->items.empty();
}

bool MediaOverlay::busy() const {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    return worker_busy_ || !requests_.empty();
}

const MediaItem* MediaOverlay::current_item() const {
    if (!presentation_ || presentation_->items.empty() || index_ < 0 ||
        index_ >= static_cast<int>(presentation_->items.size())) {
        return nullptr;
    }
    return &presentation_->items[static_cast<std::size_t>(index_)];
}

const MediaItem* MediaOverlay::find_item(const std::string& id) const {
    if (!presentation_) {
        return nullptr;
    }
    for (const MediaItem& item : presentation_->items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

ImageHandle MediaOverlay::find_image(const std::string& key) const {
    for (const CachedImage& image : images_) {
        if (image.key == key) {
            return image.handle;
        }
    }
    return kInvalidImage;
}

void MediaOverlay::clear_images() {
    for (CachedImage& image : images_) {
        if (image.handle != kInvalidImage) {
            canvas_.release_image(image.handle);
        }
    }
    images_.clear();
}

void MediaOverlay::clear_work() {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    requests_.clear();
    results_.clear();
}

void MediaOverlay::evict_if_needed() {
    while (images_.size() >= kMaxCachedImages) {
        auto oldest = std::min_element(
            images_.begin(), images_.end(),
            [](const CachedImage& left, const CachedImage& right) {
                return left.last_used < right.last_used;
            });
        if (oldest == images_.end()) {
            break;
        }
        if (oldest->handle != kInvalidImage) {
            canvas_.release_image(oldest->handle);
        }
        images_.erase(oldest);
    }
}

void MediaOverlay::store_result(FetchResult result) {
    const MediaItem* item = find_item(result.key);
    if (!item) {
        return;
    }
    const std::size_t item_index =
        static_cast<std::size_t>(item - presentation_->items.data());
    if (item_index >= statuses_.size()) {
        return;
    }
    ItemStatus& status = statuses_[item_index];
    status.loading = false;

    if (!result.ok) {
        status.error = result.error.empty() ? kNoImageError : result.error;
        log::warn("media: item %s failed: %s", item->id.c_str(), status.error.c_str());
        return;
    }
    if (item->expires_at_ms > 0 && now_ms() >= item->expires_at_ms) {
        status.error = kExpiredError;
        return;
    }
    if (find_image(item->id) != kInvalidImage) {
        return;
    }
    if (result.width <= 0 || result.height <= 0 ||
        result.rgba.size() < static_cast<std::size_t>(result.width) * result.height * 4) {
        status.error = "The picture decoder returned no pixels";
        log::warn("media: item %s decoded without pixels", item->id.c_str());
        return;
    }
    const ImageHandle handle = canvas_.create_image(result.rgba.data(), result.width,
                                                    result.height);
    if (handle == kInvalidImage) {
        status.error = "Could not create the picture texture";
        log::warn("media: item %s has no texture", item->id.c_str());
        return;
    }
    evict_if_needed();
    images_.push_back(CachedImage{item->id, handle, result.width, result.height, ++use_counter_});
    status.error.clear();
    log::info("media: decoded item %s (%dx%d)", item->id.c_str(), result.width, result.height);
}

void MediaOverlay::check_expiry() {
    if (!presentation_) {
        return;
    }
    const std::int64_t now = now_ms();
    for (std::size_t i = 0; i < presentation_->items.size() && i < statuses_.size(); ++i) {
        const MediaItem& item = presentation_->items[i];
        if (item.expires_at_ms <= 0 || now < item.expires_at_ms) {
            continue;
        }
        ItemStatus& status = statuses_[i];
        if (!status.error.empty()) {
            continue;
        }
        status.loading = false;
        status.error = kExpiredError;
        for (auto image = images_.begin(); image != images_.end(); ++image) {
            if (image->key == item.id) {
                if (image->handle != kInvalidImage) {
                    canvas_.release_image(image->handle);
                }
                images_.erase(image);
                break;
            }
        }
        log::info("media: item %s expired", item.id.c_str());
    }
}

void MediaOverlay::request_current_and_next() {
    if (!presentation_) {
        return;
    }
    for (int offset = 0; offset <= 1; ++offset) {
        const int item_index = index_ + offset;
        if (item_index < 0 || item_index >= static_cast<int>(presentation_->items.size())) {
            break;
        }
        const MediaItem& item = presentation_->items[static_cast<std::size_t>(item_index)];
        ItemStatus& status = statuses_[static_cast<std::size_t>(item_index)];
        if (status.loading || !status.error.empty()) {
            continue;
        }
        if (item.expires_at_ms > 0 && now_ms() >= item.expires_at_ms) {
            status.error = kExpiredError;
            continue;
        }
        const ImageHandle cached = find_image(item.id);
        if (cached != kInvalidImage) {
            for (CachedImage& image : images_) {
                if (image.key == item.id) {
                    image.last_used = ++use_counter_;
                }
            }
            continue;
        }
        if (item.size_bytes > kMaxImageBytes) {
            status.error = "The picture is too large";
            continue;
        }
        if (is_http_url(item.url) && !http_) {
            status.error = "No HTTP client is available";
            continue;
        }
        status.loading = true;
        FetchRequest request;
        request.key = item.id;
        request.generation = generation_;
        request.max_bytes =
            item.size_bytes > 0 ? std::min(item.size_bytes, kMaxImageBytes) : kMaxImageBytes;
        if (is_http_url(item.url)) {
            request.url = item.url;
        } else {
            request.local_path = item.url;
        }
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            requests_.push_back(std::move(request));
        }
        worker_cv_.notify_one();
    }
}

void MediaOverlay::poll() {
    std::deque<FetchResult> ready;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        ready.swap(results_);
    }
    for (FetchResult& result : ready) {
        if (result.generation != generation_) {
            continue;
        }
        store_result(std::move(result));
    }
    check_expiry();
    request_current_and_next();
}

bool MediaOverlay::settled() const {
    if (!presentation_) {
        return true;
    }
    const MediaItem* item = current_item();
    if (!item) {
        return true;
    }
    if (find_image(item->id) != kInvalidImage) {
        return true;
    }
    const std::size_t item_index =
        static_cast<std::size_t>(item - presentation_->items.data());
    return item_index < statuses_.size() && !statuses_[item_index].error.empty();
}

std::string MediaOverlay::current_error() const {
    const MediaItem* item = current_item();
    if (!item || !presentation_) {
        return {};
    }
    const std::size_t item_index =
        static_cast<std::size_t>(item - presentation_->items.data());
    if (item_index >= statuses_.size()) {
        return {};
    }
    return statuses_[item_index].error;
}

std::string MediaOverlay::status_text() const {
    if (!presentation_) {
        return "closed";
    }
    const MediaItem* item = current_item();
    std::string state = "loading";
    if (item && find_image(item->id) != kInvalidImage) {
        state = "ready";
    } else if (item) {
        const std::size_t item_index =
            static_cast<std::size_t>(item - presentation_->items.data());
        if (item_index < statuses_.size() && !statuses_[item_index].error.empty()) {
            state = "error: " + statuses_[item_index].error;
        }
    }
    return "presentation=" + presentation_->id + " items=" +
           std::to_string(presentation_->items.size()) + " index=" + std::to_string(index_) +
           " state=" + state;
}

void MediaOverlay::log_state(const char* prefix) const {
    if (!presentation_) {
        log::info("%s: closed", prefix);
        return;
    }
    log::info("%s: id=%s items=%zu title=%s", prefix, presentation_->id.c_str(),
              presentation_->items.size(),
              presentation_->title.empty() ? "(none)" : presentation_->title.c_str());
}

void MediaOverlay::draw(Canvas& canvas, Rect stage) const {
    if (!visible() || stage.w <= 0.0f || stage.h <= 0.0f) {
        return;
    }
    const MediaItem* item = current_item();
    if (!item) {
        return;
    }

    const Color background = hex(0x0b0d0b);
    const Color foreground = hex(0xf6fff5);
    const Color muted = hex(0x9ca49c);
    const Color secondary = hex(0x232b23);
    canvas.fill_rect(stage, background);

    const float header_height = std::clamp(stage.h * 0.185f, 56.0f, 96.0f);
    const float pad = std::clamp(stage.w * 0.03125f, 18.0f, 40.0f);
    const float title_size = std::clamp(stage.h * 0.058f, 18.0f, 30.0f);
    const float close_size = std::clamp(stage.h * 0.046f, 16.0f, 24.0f);
    const float close_height = std::clamp(header_height * 0.83f, 44.0f, 80.0f);
    const float close_padding = std::clamp(stage.w * 0.0375f, 20.0f, 48.0f);
    const float close_width = canvas.measure_text(close_size, "Close") + close_padding * 2.0f;
    const Rect close_rect{stage.x + stage.w - pad - close_width,
                          stage.y + (header_height - close_height) * 0.5f, close_width,
                          close_height};
    canvas.fill_rounded_rect(close_rect, close_height * 0.5f, secondary);
    canvas.text(close_rect.center_x(), close_rect.center_y() + close_size * 0.36f, close_size,
                foreground, TextAlign::Center, "Close");

    std::string title = presentation_->title;
    if (title.empty()) {
        title = item->caption;
    }
    if (title.empty()) {
        title = "Puck's image";
    }
    const float title_max = close_rect.x - pad - (stage.x + pad);
    title = fit_text(canvas, title_size, title, title_max);
    canvas.text(stage.x + pad, stage.y + header_height * 0.5f, title_size, foreground,
                TextAlign::Left, title);

    const Rect frame{stage.x + pad, stage.y + header_height, stage.w - pad * 2.0f,
                     stage.h - header_height - pad * 0.5f};
    const bool has_navigation = presentation_->items.size() > 1;
    const float nav_radius = std::clamp(frame.h * 0.10f, 28.0f, 48.0f);
    const float nav_margin = std::max(12.0f, pad * 0.6f);
    const float base_inset = std::min(frame.w * 0.10f, 140.0f);
    const float content_inset =
        has_navigation ? std::max(base_inset, nav_margin + nav_radius * 2.0f + 8.0f)
                       : base_inset;
    const Rect content{frame.x + content_inset, frame.y + pad * 0.25f,
                       frame.w - content_inset * 2.0f, frame.h - pad * 0.75f};

    const ImageHandle image = find_image(item->id);
    const std::size_t item_index =
        static_cast<std::size_t>(item - presentation_->items.data());
    const std::string error =
        item_index < statuses_.size() ? statuses_[item_index].error : std::string{};
    int image_width = 0;
    int image_height = 0;
    for (const CachedImage& cached : images_) {
        if (cached.key == item->id) {
            image_width = cached.width;
            image_height = cached.height;
            break;
        }
    }
    if (image != kInvalidImage) {
        canvas.draw_image(image, fit_rect(content, image_width, image_height), 1.0f);
    } else if (!error.empty()) {
        const float card_width = std::min(content.w * 0.72f, 640.0f);
        const float card_height = std::min(content.h * 0.52f, 240.0f);
        const Rect card{content.center_x() - card_width * 0.5f,
                        content.center_y() - card_height * 0.5f, card_width, card_height};
        canvas.fill_rounded_rect(card, 18.0f, hex(0x1a211a));
        const std::string heading = item->caption.empty() ? "Could not load the picture" : item->caption;
        canvas.text(card.center_x(), card.y + card.h * 0.34f, 24.0f, foreground,
                    TextAlign::Center, fit_text(canvas, 24.0f, heading, card.w * 0.86f));
        const std::vector<std::string> lines =
            wrap_text(canvas, 17.0f, error, card.w * 0.82f, 2);
        float line_y = card.y + card.h * 0.34f + 34.0f;
        for (const std::string& line : lines) {
            canvas.text(card.center_x(), line_y, 17.0f, muted, TextAlign::Center, line);
            line_y += 22.0f;
        }
    } else {
        canvas.text(content.center_x(), content.center_y(), 22.0f, muted, TextAlign::Center,
                    "Loading picture");
    }

    if (has_navigation) {
        const float center_y = frame.center_y();
        const float left_x = frame.x + nav_margin + nav_radius;
        const float right_x = frame.x + frame.w - nav_margin - nav_radius;
        const bool has_previous = index_ > 0;
        const bool has_next = index_ + 1 < static_cast<int>(presentation_->items.size());
        canvas.fill_circle(left_x, center_y, nav_radius,
                           hex(0x232b23, has_previous ? 1.0f : 0.3f));
        canvas.fill_circle(right_x, center_y, nav_radius,
                           hex(0x232b23, has_next ? 1.0f : 0.3f));
        draw_chevron(canvas, left_x, center_y, nav_radius * 1.4f, true,
                     hex(0xf6fff5, has_previous ? 1.0f : 0.35f));
        draw_chevron(canvas, right_x, center_y, nav_radius * 1.4f, false,
                     hex(0xf6fff5, has_next ? 1.0f : 0.35f));
    }
}

void MediaOverlay::worker_main() {
    for (;;) {
        FetchRequest request;
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
            if (stopping_) {
                return;
            }
            request = std::move(requests_.front());
            requests_.pop_front();
            worker_busy_ = true;
        }
        FetchResult result = fetch(request);
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_busy_ = false;
            if (stopping_) {
                return;
            }
            while (results_.size() >= kMaxQueuedResults) {
                results_.pop_front();
            }
            results_.push_back(std::move(result));
        }
    }
}

MediaOverlay::FetchResult MediaOverlay::fetch(const FetchRequest& request) {
    FetchResult result;
    result.key = request.key;
    result.generation = request.generation;

    std::vector<std::uint8_t> bytes;
    std::string body;
    std::string error;
    if (!request.local_path.empty()) {
        if (!read_file(request.local_path, request.max_bytes, &bytes, &error)) {
            result.error = error;
            return result;
        }
    } else if (http_) {
        HttpRequest http_request;
        http_request.url = request.url;
        http_request.headers.push_back(HttpHeader{"Accept", "image/webp,image/png,image/jpeg,image/*"});
        http_request.headers.push_back(
            HttpHeader{"Referer", strip_trailing_slash(app_origin_) + "/car"});
        HttpResponse response = http_->request(http_request, &error);
        if (!error.empty()) {
            result.error = "The picture download failed";
            log::warn("media: fetch for item %s failed: %s", request.key.c_str(), error.c_str());
            return result;
        }
        if (response.status < 200 || response.status >= 300) {
            result.error = "The picture request returned HTTP " + std::to_string(response.status);
            return result;
        }
        const std::string length = header_value(response, "Content-Length");
        if (!length.empty()) {
            try {
                if (std::stoll(length) > request.max_bytes) {
                    result.error = "The picture is too large";
                    return result;
                }
            } catch (...) {
            }
        }
        if (static_cast<std::int64_t>(response.body.size()) > request.max_bytes) {
            result.error = "The picture is too large";
            return result;
        }
        body = std::move(response.body);
    } else {
        result.error = "No HTTP client is available";
        return result;
    }

    const std::uint8_t* data = request.local_path.empty()
                                   ? reinterpret_cast<const std::uint8_t*>(body.data())
                                   : bytes.data();
    const std::size_t size = request.local_path.empty() ? body.size() : bytes.size();
    if (size == 0) {
        result.error = "The picture is empty";
        return result;
    }
    if (!decode_image(data, size, &result.width, &result.height, &result.rgba, &error)) {
        result.error = error.empty() ? kNoImageError : error;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace pk
