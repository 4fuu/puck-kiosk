#include "app/assets.h"

#include "core/log.h"
#include "core/sha256.h"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pk {
namespace {

constexpr const char* kUserAgent = "puck-kiosk/0.1";

bool valid_sha256(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool safe_relative_path(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == ".." || component == ".") {
            return false;
        }
    }
    return true;
}

bool write_file(const std::filesystem::path& path, const std::string& body, std::string* error) {
    std::error_code code;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), code);
        if (code) {
            if (error) {
                *error = "cannot create " + path.parent_path().string();
            }
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    output.flush();
    if (!output) {
        if (error) {
            *error = "cannot write " + path.string();
        }
        return false;
    }
    return true;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

// The asset CDN stores these files gzip-compressed and returns the compressed
// bytes even for "Accept-Encoding: identity". The Windows curl build has no
// gzip support, so decode the body here before verifying the manifest hash.
bool decode_body(const std::string& input, const std::string& encoding, std::string* output,
                 std::string* error) {
    if (encoding.find("gzip") == std::string::npos &&
        encoding.find("deflate") == std::string::npos) {
        if (!encoding.empty() && encoding != "identity") {
            if (error) {
                *error = "unsupported content encoding: " + encoding;
            }
            return false;
        }
        *output = input;
        return true;
    }

    const int window_bits =
        encoding.find("gzip") != std::string::npos ? 16 + MAX_WBITS : MAX_WBITS;
    z_stream stream{};
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        if (error) {
            *error = "cannot start gzip decompression";
        }
        return false;
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    std::array<char, 64 * 1024> buffer{};
    std::string decoded;
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = buffer.size() - stream.avail_out;
        decoded.append(buffer.data(), produced);
        if (decoded.size() > 256u * 1024u * 1024u) {
            inflateEnd(&stream);
            if (error) {
                *error = "decoded asset is too large";
            }
            return false;
        }
    }
    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        if (error) {
            *error = "gzip decompression failed";
        }
        return false;
    }
    *output = std::move(decoded);
    return true;
}

}  // namespace

AssetFetcher::AssetFetcher(HttpClient& http, std::string cache_root, std::string manifest_path)
    : http_(http), cache_root_(std::move(cache_root)), manifest_path_(std::move(manifest_path)) {}

AssetFetcher::~AssetFetcher() {
    wait();
}

void AssetFetcher::start() {
    if (started_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void AssetFetcher::wait() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

AssetFetchStatus AssetFetcher::status() const {
    AssetFetchStatus status;
    status.finished = finished_.load();
    status.ok = ok_.load();
    status.completed = completed_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    status.total = total_;
    status.current = current_;
    status.error = error_;
    return status;
}

bool AssetFetcher::load_manifest(std::vector<Asset>* assets, std::string* error) const {
    std::ifstream input(manifest_path_, std::ios::binary);
    if (!input) {
        if (error) {
            *error = "cannot open " + manifest_path_;
        }
        return false;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    auto manifest = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object() || !manifest.contains("assets") ||
        !manifest["assets"].is_array()) {
        if (error) {
            *error = "invalid asset manifest";
        }
        return false;
    }

    std::vector<Asset> parsed;
    int index = 0;
    for (const auto& entry : manifest["assets"]) {
        Asset asset;
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() ||
            !entry.contains("url") || !entry["url"].is_string() || !entry.contains("sha256") ||
            !entry["sha256"].is_string() || !entry.contains("bytes") ||
            !entry["bytes"].is_number_integer()) {
            if (error) {
                *error = "asset " + std::to_string(index) + " is missing a field";
            }
            return false;
        }
        asset.path = entry["path"].get<std::string>();
        asset.url = entry["url"].get<std::string>();
        asset.sha256 = entry["sha256"].get<std::string>();
        asset.bytes = entry["bytes"].get<long long>();
        if (!safe_relative_path(asset.path)) {
            if (error) {
                *error = "asset " + std::to_string(index) + " has an unsafe path";
            }
            return false;
        }
        if (asset.url.rfind("https://", 0) != 0 && asset.url.rfind("http://", 0) != 0) {
            if (error) {
                *error = "asset " + asset.path + " has an unsupported URL";
            }
            return false;
        }
        if (!valid_sha256(asset.sha256) || asset.bytes <= 0) {
            if (error) {
                *error = "asset " + asset.path + " has invalid verification data";
            }
            return false;
        }
        parsed.push_back(std::move(asset));
        ++index;
    }
    if (parsed.empty()) {
        if (error) {
            *error = "the asset manifest lists no files";
        }
        return false;
    }
    *assets = std::move(parsed);
    return true;
}

bool AssetFetcher::verify_cached(const Asset& asset, std::string* error) const {
    const std::filesystem::path path = std::filesystem::path(cache_root_) / asset.path;
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || static_cast<long long>(size) != asset.bytes) {
        return false;
    }
    std::string digest;
    if (!sha256_file(path.string(), &digest, error)) {
        return false;
    }
    return digest == asset.sha256;
}

bool AssetFetcher::download(const Asset& asset, std::string* error) {
    HttpRequest request;
    request.method = "GET";
    request.url = asset.url;
    request.headers = {
        {"Accept", "*/*"},
        {"Accept-Encoding", "identity"},
        {"User-Agent", kUserAgent},
    };
    std::string request_error;
    const HttpResponse response = http_.request(request, &request_error);
    if (!request_error.empty()) {
        if (error) {
            *error = asset.path + ": " + request_error;
        }
        return false;
    }
    if (response.status != 200) {
        if (error) {
            *error = asset.path + ": HTTP " + std::to_string(response.status);
        }
        return false;
    }
    std::string encoding;
    for (const HttpHeader& header : response.headers) {
        if (lowercase(header.name) == "content-encoding") {
            encoding = lowercase(header.value);
        }
    }
    std::string body;
    std::string decode_error;
    if (!decode_body(response.body, encoding, &body, &decode_error)) {
        if (error) {
            *error = asset.path + ": " + decode_error;
        }
        return false;
    }
    if (static_cast<long long>(body.size()) != asset.bytes) {
        if (error) {
            *error = asset.path + ": expected " + std::to_string(asset.bytes) + " bytes, got " +
                     std::to_string(body.size());
        }
        return false;
    }
    const std::string digest = sha256_hex(body);
    if (digest != asset.sha256) {
        if (error) {
            *error = asset.path + ": sha256 mismatch";
        }
        return false;
    }

    const std::filesystem::path target = std::filesystem::path(cache_root_) / asset.path;
    auto temporary = target;
    temporary += ".fetch";
    if (!write_file(temporary, body, error)) {
        std::error_code code;
        std::filesystem::remove(temporary, code);
        return false;
    }
    std::error_code code;
    std::filesystem::remove(target, code);
    code.clear();
    std::filesystem::rename(temporary, target, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        if (error) {
            *error = asset.path + ": cannot replace the cache file";
        }
        return false;
    }
    return true;
}

void AssetFetcher::run() {
    std::vector<Asset> assets;
    std::string error;
    if (!load_manifest(&assets, &error)) {
        log::warn("assets: %s; using the placeholder Puck", error.c_str());
        finish(false, error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        total_ = static_cast<int>(assets.size());
    }

    for (std::size_t index = 0; index < assets.size(); ++index) {
        const Asset& asset = assets[index];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = asset.path;
        }
        if (verify_cached(asset, &error)) {
            log::info("assets: cached %s", asset.path.c_str());
            completed_.store(static_cast<int>(index + 1));
            continue;
        }
        log::info("assets: fetching %s", asset.path.c_str());
        if (!download(asset, &error)) {
            log::warn("assets: %s; using the cached files", error.c_str());
            finish(false, error);
            return;
        }
        log::info("assets: fetched %s (%lld bytes, sha256 ok)", asset.path.c_str(), asset.bytes);
        completed_.store(static_cast<int>(index + 1));
    }
    finish(true, {});
}

void AssetFetcher::finish(bool ok, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
        current_.clear();
    }
    ok_.store(ok);
    finished_.store(true);
    if (ok) {
        const AssetFetchStatus status = this->status();
        log::info("assets: %d/%d verified (cache %s)", status.completed, status.total,
                  cache_root_.c_str());
    }
}

}  // namespace pk
