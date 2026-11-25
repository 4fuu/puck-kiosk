#include "app/pairing.h"

#include "core/log.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <qrencode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pk {
namespace {

constexpr const char* kUserAgent = "puck-kiosk/0.1";

// Browsers open about six connections per host. Sibling chunks are fetched in
// batches of this size so one slow chunk does not serialize the rest.
constexpr std::size_t kFetchBatch = 6;
constexpr std::size_t kMaxChunkFetches = 48;

using Clock = std::chrono::steady_clock;

long long elapsed_ms(Clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
}

std::string strip_trailing_slash(std::string value) {
    while (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string url_encode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0f]);
        }
    }
    return encoded;
}

std::string url_decode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            const std::string byte = value.substr(i + 1, 2);
            decoded.push_back(static_cast<char>(std::strtol(byte.c_str(), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::string origin_of(const std::string& url) {
    std::regex re(R"(^([A-Za-z][A-Za-z0-9+.-]*://[^/?#]+))");
    std::smatch match;
    if (std::regex_search(url, match, re)) {
        return match[1].str();
    }
    return url;
}

std::string resolve_url(const std::string& base, const std::string& relative) {
    if (relative.rfind("http://", 0) == 0 || relative.rfind("https://", 0) == 0) {
        return relative;
    }
    const auto scheme_end = base.find("://");
    if (scheme_end == std::string::npos) {
        return relative;
    }
    const auto path_start = base.find('/', scheme_end + 3);
    const std::string origin = path_start == std::string::npos ? base : base.substr(0, path_start);
    std::string path = path_start == std::string::npos ? "/" : base.substr(path_start);
    if (relative.empty()) {
        return origin + path;
    }
    if (relative.front() == '/') {
        path = relative;
    } else {
        const auto slash = path.rfind('/');
        path = (slash == std::string::npos ? "/" : path.substr(0, slash + 1)) + relative;
    }

    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    std::string normalized = origin;
    for (const auto& segment : segments) {
        normalized += "/";
        normalized += segment;
    }
    return normalized;
}

bool http_get(HttpClient& http, const std::string& url, std::string* body, std::string* error) {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers = {
        {"Accept", "text/html,application/javascript,*/*"},
        {"User-Agent", kUserAgent},
    };
    auto response = http.request(request, error);
    if (error && !error->empty()) {
        return false;
    }
    if (response.status != 200) {
        if (error) {
            *error = "HTTP " + std::to_string(response.status) + " for " + redact_url(url);
        }
        return false;
    }
    if (body) {
        *body = std::move(response.body);
    }
    return true;
}

HttpRequest make_get_request(const std::string& url) {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers = {
        {"Accept", "text/html,application/javascript,*/*"},
        {"User-Agent", kUserAgent},
    };
    return request;
}

std::string extract_remote_hash(const std::string& content) {
    std::regex re(R"(([A-Za-z0-9_-]{3,})/claimDevicePairing)");
    std::smatch match;
    if (std::regex_search(content, match, re)) {
        return match[1].str();
    }
    return {};
}

bool slice_object(const std::string& text, std::size_t open, std::string* object) {
    if (open >= text.size() || text[open] != '{') {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    char quote = '\0';
    for (std::size_t i = open; i < text.size(); ++i) {
        const char character = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                in_string = false;
            }
            continue;
        }
        if (character == '"' || character == '\'' || character == '`') {
            in_string = true;
            quote = character;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
            if (depth == 0) {
                *object = text.substr(open, i - open + 1);
                return true;
            }
        }
    }
    return false;
}

std::string unescape_json_string(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            decoded.push_back(value[i]);
            continue;
        }
        const char next = value[++i];
        switch (next) {
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'u': {
                if (i + 4 < value.size()) {
                    const std::string hex = value.substr(i + 1, 4);
                    const auto code = static_cast<unsigned>(std::strtoul(hex.c_str(), nullptr, 16));
                    if (code < 0x80) {
                        decoded.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        decoded.push_back(static_cast<char>(0xc0 | (code >> 6)));
                        decoded.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        decoded.push_back(static_cast<char>(0xe0 | (code >> 12)));
                        decoded.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        decoded.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                    i += 4;
                }
                break;
            }
            default: decoded.push_back(next); break;
        }
    }
    return decoded;
}

bool extract_object_field(const std::string& parent, const std::string& key, std::string* value) {
    const std::regex key_re("(?:^|[,{])\\s*(?:\"" + key + "\"|" + key + ")\\s*:");
    std::smatch match;
    if (!std::regex_search(parent, match, key_re)) {
        return false;
    }
    std::size_t position = static_cast<std::size_t>(match.position() + match.length());
    while (position < parent.size() &&
           std::isspace(static_cast<unsigned char>(parent[position]))) {
        ++position;
    }
    return slice_object(parent, position, value);
}

bool extract_string_field(const std::string& parent, const std::string& key, std::string* value) {
    const std::regex value_re("(?:^|[,{])\\s*(?:\"" + key + "\"|" + key +
                              ")\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
    std::smatch match;
    if (!std::regex_search(parent, match, value_re)) {
        return false;
    }
    if (value) {
        *value = unescape_json_string(match[1].str());
    }
    return true;
}

bool extract_bool_field(const std::string& parent, const std::string& key, bool* value) {
    const std::regex value_re("(?:^|[,{])\\s*(?:\"" + key + "\"|" + key +
                              ")\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(parent, match, value_re)) {
        return false;
    }
    if (value) {
        *value = match[1].str() == "true";
    }
    return true;
}

bool extract_car_object(const std::string& html, std::string* car, std::string* error) {
    const std::regex marker_re(R"(type\s*:\s*"data"\s*,\s*data\s*:\s*\{)");
    std::smatch marker;
    std::size_t data_open = std::string::npos;
    for (std::sregex_iterator it(html.begin(), html.end(), marker_re), end; it != end; ++it) {
        data_open = static_cast<std::size_t>(it->position() + it->length() - 1);
    }
    if (data_open == std::string::npos) {
        if (error) {
            *error = "cannot find the car page data payload";
        }
        return false;
    }

    const std::string data = html.substr(data_open);
    const std::regex car_re("(?:^|[,{])\\s*(?:\"car\"|car)\\s*:");
    std::smatch car_match;
    if (!std::regex_search(data, car_match, car_re)) {
        if (error) {
            *error = "the car page has no car field";
        }
        return false;
    }
    std::size_t position = static_cast<std::size_t>(car_match.position() + car_match.length());
    while (position < data.size() && std::isspace(static_cast<unsigned char>(data[position]))) {
        ++position;
    }
    if (data.compare(position, 4, "null") == 0) {
        if (error) {
            *error = "car data is null; the session is not authenticated";
        }
        return false;
    }
    if (position >= data.size() || data[position] != '{') {
        if (error) {
            *error = "car data is not an object";
        }
        return false;
    }
    if (!slice_object(data, position, car)) {
        if (error) {
            *error = "cannot parse the car data object";
        }
        return false;
    }
    return true;
}

nlohmann::json decode_devalue_index(const nlohmann::json& values, int index, int depth);

nlohmann::json decode_devalue_object(const nlohmann::json& value, const nlohmann::json& values,
                                     int depth) {
    nlohmann::json decoded = nlohmann::json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it.value().is_number_integer()) {
            return nullptr;
        }
        decoded[it.key()] = decode_devalue_index(values, it.value().get<int>(), depth + 1);
    }
    return decoded;
}

nlohmann::json decode_devalue_index(const nlohmann::json& values, int index, int depth) {
    if (depth > 64 || index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        return nullptr;
    }
    const auto& value = values[index];
    if (value.is_string() || value.is_boolean() || value.is_null() || value.is_number()) {
        return value;
    }
    if (value.is_object()) {
        return decode_devalue_object(value, values, depth);
    }
    if (!value.is_array()) {
        return nullptr;
    }
    if (!value.empty() && value[0].is_string()) {
        const std::string tag = value[0].get<std::string>();
        if (tag == "null") {
            nlohmann::json decoded = nlohmann::json::object();
            for (std::size_t i = 1; i + 1 < value.size(); i += 2) {
                if (!value[i].is_string() || !value[i + 1].is_number_integer()) {
                    return nullptr;
                }
                decoded[value[i].get<std::string>()] =
                    decode_devalue_index(values, value[i + 1].get<int>(), depth + 1);
            }
            return decoded;
        }
        if (tag == "Object" && value.size() >= 2 && value[1].is_number_integer()) {
            return decode_devalue_index(values, value[1].get<int>(), depth + 1);
        }
        if (tag == "Date" && value.size() >= 2) {
            return value[1];
        }
        return nullptr;
    }
    nlohmann::json decoded = nlohmann::json::array();
    for (const auto& item : value) {
        if (!item.is_number_integer()) {
            return nullptr;
        }
        decoded.push_back(decode_devalue_index(values, item.get<int>(), depth + 1));
    }
    return decoded;
}

bool parse_car_page(const std::string& page, CarSession* parsed, std::string* error) {
    std::string car;
    if (!extract_car_object(page, &car, error)) {
        return false;
    }
    std::string puck;
    if (!extract_object_field(car, "puck", &puck)) {
        if (error) {
            *error = "car.puck is missing";
        }
        return false;
    }
    std::string thread;
    if (!extract_object_field(puck, "thread", &thread)) {
        if (error) {
            *error = "car.puck.thread is missing";
        }
        return false;
    }

    CarSession next;
    if (!extract_string_field(thread, "id", &next.thread_id) || next.thread_id.empty()) {
        if (error) {
            *error = "car.puck.thread.id is missing";
        }
        return false;
    }

    std::string actor_config;
    if (!extract_object_field(puck, "threadActorConfig", &actor_config)) {
        if (!extract_object_field(page, "threadActorConfig", &actor_config)) {
            if (error) {
                *error = "threadActorConfig is missing";
            }
            return false;
        }
        log::warn("car.puck.threadActorConfig is missing; using the page-level actor config");
    }
    if (!extract_string_field(actor_config, "baseURL", &next.actor_base_url) ||
        next.actor_base_url.empty()) {
        if (error) {
            *error = "threadActorConfig.baseURL is missing";
        }
        return false;
    }
    extract_string_field(actor_config, "ampURL", &next.actor_amp_url);
    extract_string_field(actor_config, "poolName", &next.actor_pool);
    extract_string_field(actor_config, "capability", &next.capability);
    extract_bool_field(actor_config, "requiresSudoForWrite", &next.requires_sudo_for_write);

    *parsed = std::move(next);
    return true;
}

bool decode_devalue_data(const std::string& data, nlohmann::json* result, std::string* error) {
    auto values = nlohmann::json::parse(data, nullptr, false);
    if (values.is_discarded() || !values.is_array() || values.empty()) {
        if (error) {
            *error = "pairing response payload is not a devalue array";
        }
        return false;
    }
    nlohmann::json decoded = decode_devalue_index(values, 0, 0);
    if (decoded.is_null()) {
        if (error) {
            *error = "cannot decode the pairing response payload";
        }
        return false;
    }
    if (decoded.is_object() && decoded.contains("_")) {
        decoded = decoded["_"];
    }
    if (!decoded.is_object()) {
        if (error) {
            *error = "pairing response payload is not an object";
        }
        return false;
    }
    *result = std::move(decoded);
    return true;
}

void redact_json_value(nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string& key = it.key();
            if (key == "wsToken" || key == "token" || key == "authorization" || key == "cookie" ||
                key == "baseURL" || key == "actorBaseURL" || key == "sdp" || key == "audioBase64" ||
                key == "dataBase64") {
                it.value() = "[redacted]";
            } else if (key == "url" && it.value().is_string()) {
                it.value() = redact_url(it.value().get<std::string>());
            } else {
                redact_json_value(it.value());
            }
        }
    } else if (value.is_array()) {
        for (auto& item : value) {
            redact_json_value(item);
        }
    }
}

std::vector<std::uint8_t> random_bytes(std::size_t size) {
    std::vector<std::uint8_t> buffer(size);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        std::random_device device;
        for (auto& byte : buffer) {
            byte = static_cast<std::uint8_t>(device());
        }
    }
    return buffer;
}

std::string random_bytes_as_base64(std::size_t bytes) {
    const auto buffer = random_bytes(bytes);
    return base64url_encode(buffer.data(), buffer.size());
}

}  // namespace

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((size + 2) / 3) * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t first = data[i];
        const std::uint32_t second = i + 1 < size ? data[i + 1] : 0;
        const std::uint32_t third = i + 2 < size ? data[i + 2] : 0;
        const std::uint32_t value = (first << 16) | (second << 8) | third;
        encoded.push_back(alphabet[(value >> 18) & 0x3f]);
        encoded.push_back(alphabet[(value >> 12) & 0x3f]);
        encoded.push_back(i + 1 < size ? alphabet[(value >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < size ? alphabet[value & 0x3f] : '=');
    }
    return encoded;
}

std::string base64url_encode(const std::uint8_t* data, std::size_t size) {
    std::string encoded = base64_encode(data, size);
    for (auto& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

std::string random_base64url(std::size_t bytes) {
    return random_bytes_as_base64(bytes);
}

std::string random_uuid() {
    auto bytes = random_bytes(16);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    char text[37];
    std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                  bytes[15]);
    return text;
}

QrCode make_qr_code(const std::string& text) {
    QrCode code;
    QRcode* encoded = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!encoded) {
        return code;
    }
    code.size = encoded->width;
    code.modules.resize(static_cast<std::size_t>(code.size) * code.size);
    for (std::size_t i = 0; i < code.modules.size(); ++i) {
        code.modules[i] = (encoded->data[i] & 1u) != 0 ? 1 : 0;
    }
    QRcode_free(encoded);
    return code;
}

PairingSession::PairingSession(HttpClient& http, SecureStore* store, PairingOptions options)
    : http_(http), store_(store), options_(std::move(options)) {
    options_.origin = strip_trailing_slash(options_.origin);
}

PairingSession::~PairingSession() {
    stop_polling();
}

void PairingSession::start_polling() {
    std::lock_guard<std::mutex> lifecycle(poll_lifecycle_mutex_);
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(poll_wait_mutex_);
        if (poll_thread_.joinable()) {
            if (poll_running_) {
                return;
            }
            // A worker that stopped on a terminal result is joinable but no
            // longer polling. restart() must be able to replace it.
            finished = std::move(poll_thread_);
        }
    }
    // Finish any old-round cleanup before a replacement worker can start its
    // first claim. This keeps an old session DELETE ahead of the new session.
    if (finished.joinable()) {
        finished.join();
    }
    {
        std::lock_guard<std::mutex> lock(poll_wait_mutex_);
        poll_stop_ = false;
        poll_wakeup_ = false;
        poll_running_ = true;
        poll_thread_ = std::thread([this] { run_poll_loop(); });
    }
}

void PairingSession::stop_polling() {
    std::lock_guard<std::mutex> lifecycle(poll_lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(poll_wait_mutex_);
        poll_stop_ = true;
        poll_wakeup_ = true;
    }
    poll_wake_.notify_all();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(poll_wait_mutex_);
        if (poll_thread_.joinable()) {
            thread = std::move(poll_thread_);
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
}

bool PairingSession::take_update(PairingUpdate* update) {
    std::lock_guard<std::mutex> lock(update_mutex_);
    if (!latest_.valid) {
        return false;
    }
    if (update) {
        *update = latest_;
    }
    latest_ = PairingUpdate{};
    return true;
}

bool PairingSession::can_retry() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == PairingState::Scanned || state_ == PairingState::Failed ||
           state_ == PairingState::Expired || paired_unacknowledged_ ||
           !last_poll_error_.empty();
}

bool PairingSession::paired_pending() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == PairingState::Paired && paired_unacknowledged_;
}

bool PairingSession::acknowledge_paired() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != PairingState::Paired || !paired_unacknowledged_ ||
        paired_generation_ != challenge_generation_) {
        return false;
    }
    paired_unacknowledged_ = false;
    return true;
}

bool PairingSession::expire(const std::string& message) {
    PairingStatus status;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == PairingState::Paired && paired_unacknowledged_) {
            discard_pending_ = true;
        }
        state_ = PairingState::Expired;
        last_code_.clear();
        last_poll_error_ = message;
        status.state = state_;
        status.message = message;
        publish_update_locked(status);
    }
    {
        std::lock_guard<std::mutex> lock(poll_wait_mutex_);
        poll_wakeup_ = true;
    }
    poll_wake_.notify_all();
    log::info("pairing: %s", message.c_str());
    return true;
}

std::string PairingSession::pairing_url() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pairing_url_;
}

std::string PairingSession::remote_hash() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return remote_hash_;
}

QrCode PairingSession::qr() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return qr_;
}

int PairingSession::challenge_generation() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return challenge_generation_;
}

bool PairingSession::restore(CarSession* saved, std::string* error) {
    if (error) {
        error->clear();
    }
    bool loaded = false;
    std::string cookies;
    if (store_ && store_->load("http.cookies", &cookies)) {
        http_.import_cookies(cookies);
        loaded = true;
    }
    std::string session_json;
    if (store_ && store_->load("car.session", &session_json)) {
        auto json = nlohmann::json::parse(session_json, nullptr, false);
        if (!json.is_discarded() && json.is_object()) {
            try {
                if (saved) {
                    saved->thread_id = json.value("thread_id", std::string{});
                    saved->actor_base_url = json.value("actor_base_url", std::string{});
                    saved->actor_amp_url = json.value("actor_amp_url", std::string{});
                    saved->actor_pool = json.value("actor_pool", std::string{});
                    saved->capability = json.value("capability", std::string{});
                    saved->requires_sudo_for_write = json.value("requires_sudo_for_write", false);
                }
                loaded = true;
            } catch (const std::exception&) {
                if (error) {
                    *error = "saved car session is malformed";
                }
            }
        }
    }
    if (!loaded && error) {
        *error = "no saved session";
    }
    return loaded;
}

bool PairingSession::start(std::string* error) {
    if (error) {
        error->clear();
    }
    const auto started = Clock::now();
    // A new pairing round must not fall back to the previous user's saved
    // session if setup or loading fails before persist().
    if (store_) {
        store_->erase("car.session");
        store_->erase("http.cookies");
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = PairingState::Idle;
        remote_hash_.clear();
        last_poll_error_.clear();
        paired_generation_ = 0;
        paired_unacknowledged_ = false;
        discard_pending_ = false;
    }
    if (!resolve_remote_hash(error)) {
        return false;
    }
    const auto hash_done = Clock::now();
    if (!regenerate_challenge(error)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // The caller renders this first QR from the getters; the first poll
        // snapshot should not claim the challenge changed again.
        published_generation_ = challenge_generation_;
    }
    log::info("startup: challenge generated in %lld ms", elapsed_ms(hash_done));
    log::info("startup: pairing prepared in %lld ms", elapsed_ms(started));
    return true;
}

void PairingSession::set_remote_hash(std::string hash) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    remote_hash_ = std::move(hash);
}

bool PairingSession::resolve_remote_hash(std::string* error) {
    const auto started = Clock::now();
    std::string page;
    if (!cached_car_page_.empty()) {
        page = std::move(cached_car_page_);
        cached_car_page_.clear();
        log::info("startup: /car page reused from session validation");
    } else {
        const auto page_started = Clock::now();
        std::string page_error;
        if (!http_get(http_, options_.origin + "/car", &page, &page_error)) {
            log::warn("cannot inspect /car (%s); using fallback remote hash %s",
                      page_error.c_str(), options_.fallback_hash.c_str());
            set_remote_hash(options_.fallback_hash);
            return true;
        }
        log::info("startup: GET /car %lld ms", elapsed_ms(page_started));
    }

    // A build that inlines the remote reference into the page saves the whole
    // module walk; check the page before requesting any chunk.
    const std::string page_hash = extract_remote_hash(page);
    if (!page_hash.empty()) {
        set_remote_hash(page_hash);
        log::info("resolved SvelteKit remote hash: %s", page_hash.c_str());
        log::info("startup: remote hash resolved from /car in %lld ms", elapsed_ms(started));
        return true;
    }

    std::vector<std::string> node_urls;
    std::unordered_set<std::string> seen;
    const std::regex node_re(R"((/_app/immutable/nodes/[A-Za-z0-9._-]+\.js))");
    for (std::sregex_iterator it(page.begin(), page.end(), node_re), end; it != end; ++it) {
        const std::string url = options_.origin + it->str(1);
        if (seen.insert(url).second) {
            node_urls.push_back(url);
        }
    }

    const std::regex import_re(R"((?:from\s*|import\s*\(\s*)["']([^"']+\.js)["'])");
    std::vector<std::string> car_imports;
    std::vector<std::string> other_imports;
    std::size_t node_requests = 0;

    auto fetch = [&](const std::vector<std::string>& urls) {
        std::vector<HttpRequest> requests;
        requests.reserve(urls.size());
        for (const std::string& url : urls) {
            requests.push_back(make_get_request(url));
        }
        return http_.request_many(requests);
    };

    // The layout and page nodes are independent. One batch fetches them
    // together over the connection cache instead of one round trip each.
    const auto nodes_started = Clock::now();
    for (std::size_t offset = 0; offset < node_urls.size(); offset += kFetchBatch) {
        const std::size_t end = std::min(offset + kFetchBatch, node_urls.size());
        const std::vector<std::string> batch(node_urls.begin() + static_cast<std::ptrdiff_t>(offset),
                                             node_urls.begin() + static_cast<std::ptrdiff_t>(end));
        const std::vector<HttpBatchResponse> results = fetch(batch);
        node_requests += batch.size();
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (!results[i].error.empty()) {
                log::warn("cannot inspect %s: %s", redact_url(batch[i]).c_str(),
                          results[i].error.c_str());
                continue;
            }
            if (results[i].response.status != 200) {
                log::warn("cannot inspect %s: HTTP %d", redact_url(batch[i]).c_str(),
                          results[i].response.status);
                continue;
            }
            const std::string& content = results[i].response.body;
            const std::string hash = extract_remote_hash(content);
            if (!hash.empty()) {
                set_remote_hash(hash);
                log::info("resolved SvelteKit remote hash: %s", hash.c_str());
                log::info("startup: node scripts %zu requests %lld ms", node_requests,
                          elapsed_ms(nodes_started));
                log::info("startup: remote hash resolved after %zu requests, %lld ms",
                          node_requests, elapsed_ms(started));
                return true;
            }
            const bool is_car_node = content.find("/api/car/session") != std::string::npos;
            auto& imports = is_car_node ? car_imports : other_imports;
            for (std::sregex_iterator it(content.begin(), content.end(), import_re), end;
                 it != end; ++it) {
                const std::string url = resolve_url(batch[i], it->str(1));
                if (seen.find(url) == seen.end()) {
                    imports.push_back(url);
                }
            }
        }
    }
    if (node_requests > 0) {
        log::info("startup: node scripts %zu requests %lld ms", node_requests,
                  elapsed_ms(nodes_started));
    }

    std::vector<std::string> pending = car_imports.empty() ? other_imports : car_imports;
    std::size_t fetched = 0;
    std::size_t waves = 0;
    while (fetched < kMaxChunkFetches) {
        std::vector<std::string> batch;
        while (batch.size() < kFetchBatch && fetched + batch.size() < kMaxChunkFetches) {
            std::string url;
            bool took = false;
            while (!pending.empty()) {
                url = std::move(pending.back());
                pending.pop_back();
                if (seen.insert(url).second) {
                    took = true;
                    break;
                }
            }
            if (!took) {
                break;
            }
            batch.push_back(std::move(url));
        }
        if (batch.empty()) {
            break;
        }
        ++waves;
        const auto wave_started = Clock::now();
        const std::vector<HttpBatchResponse> results = fetch(batch);
        fetched += batch.size();
        std::string found;
        // Results are ordered highest priority first. Process the middle
        // first and the two ends last, so the next wave advances both the
        // first and the last branch: a depth-first walk that hedges on where
        // the hash sits in the import order.
        for (std::size_t step = 0; step < batch.size(); ++step) {
            const std::size_t i = (step + 1) % batch.size();
            if (!results[i].error.empty()) {
                log::warn("cannot inspect %s: %s", redact_url(batch[i]).c_str(),
                          results[i].error.c_str());
                continue;
            }
            if (results[i].response.status != 200) {
                log::warn("cannot inspect %s: HTTP %d", redact_url(batch[i]).c_str(),
                          results[i].response.status);
                continue;
            }
            const std::string& content = results[i].response.body;
            const std::string hash = extract_remote_hash(content);
            if (!hash.empty()) {
                found = hash;
                break;
            }
            for (std::sregex_iterator it(content.begin(), content.end(), import_re), end;
                 it != end; ++it) {
                const std::string url = resolve_url(batch[i], it->str(1));
                if (seen.find(url) == seen.end()) {
                    pending.push_back(url);
                }
            }
        }
        log::info("startup: chunk wave %zu: %zu requests %lld ms", waves, batch.size(),
                  elapsed_ms(wave_started));
        if (!found.empty()) {
            set_remote_hash(found);
            log::info("resolved SvelteKit remote hash: %s", found.c_str());
            log::info("startup: remote hash resolved after %zu requests in %zu waves, %lld ms",
                      node_requests + fetched, waves, elapsed_ms(started));
            return true;
        }
    }

    set_remote_hash(options_.fallback_hash);
    log::warn("could not resolve the SvelteKit remote hash from /car; using fallback %s",
              options_.fallback_hash.c_str());
    log::info("startup: remote hash fallback after %zu requests in %zu waves, %lld ms",
              node_requests + fetched, waves, elapsed_ms(started));
    return true;
}

bool PairingSession::regenerate_challenge(std::string* error) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return regenerate_challenge_locked(error);
}

bool PairingSession::regenerate_challenge_locked(std::string* error) {
    verifier_ = random_bytes_as_base64(32);
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(verifier_.data()), verifier_.size(), digest.data());
    challenge_ = base64url_encode(digest.data(), digest.size());
    pairing_url_ = options_.origin + "/auth/pair/car?c=" + challenge_;
    qr_ = make_qr_code(pairing_url_);
    if (qr_.empty()) {
        if (error) {
            *error = "cannot encode the pairing QR";
        }
        return false;
    }
    challenge_created_ = std::chrono::steady_clock::now();
    ++challenge_generation_;
    last_code_.clear();
    last_poll_error_.clear();
    paired_generation_ = 0;
    paired_unacknowledged_ = false;
    state_ = PairingState::Waiting;
    return true;
}

bool PairingSession::restart(std::string* error) {
    if (error) {
        error->clear();
    }
    PairingStatus status;
    bool discard = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // A successful reply can be published before the UI consumes it. A
        // retry at that moment still owns an account-creating session, so it
        // must delete that session and keep polling for the new round.
        discard = state_ == PairingState::Paired && paired_unacknowledged_;
        if (!regenerate_challenge_locked(error)) {
            return false;
        }
        discard_pending_ = discard_pending_ || discard;
        status.state = state_;
        status.message = "Waiting for scan";
        publish_update_locked(status);
    }
    {
        std::lock_guard<std::mutex> lock(poll_wait_mutex_);
        // A retry cancels a pending stop as well: an in-flight request from
        // the expired round must not end the worker before the new claim.
        poll_stop_ = false;
        poll_wakeup_ = true;
    }
    poll_wake_.notify_all();
    // The worker may have stopped on a terminal result or an unconsumed
    // Paired snapshot. Revive it so the new round is actually polled.
    start_polling();
    log::info("pairing: restarted with a fresh challenge");
    return true;
}

HttpResponse PairingSession::post_remote(const std::string& function, const std::string& body,
                                         std::string* error) {
    HttpRequest request;
    request.method = "POST";
    request.url = options_.origin + "/_app/remote/" + remote_hash_ + "/" + function;
    request.headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Origin", options_.origin},
        {"Referer", options_.origin + "/car"},
        {"x-sveltekit-pathname", "/car"},
        {"x-sveltekit-search", ""},
        {"User-Agent", kUserAgent},
    };
    request.body = body;

    auto response = http_.request(request, error);
    if (error && !error->empty()) {
        return response;
    }
    if (response.status != 200) {
        if (error) {
            *error = "pairing endpoint returned HTTP " + std::to_string(response.status);
            if (!response.body.empty()) {
                *error += ": " + redact_text(response.body, 160);
            }
        }
    }
    return response;
}

bool PairingSession::decode_pairing_response(const std::string& body, PairingStatus* status,
                                             std::string* error) {
    auto envelope = nlohmann::json::parse(body, nullptr, false);
    if (envelope.is_discarded()) {
        if (error) {
            *error = "pairing response is not JSON";
        }
        return false;
    }
    const std::string type =
        envelope.contains("type") && envelope["type"].is_string() ? envelope["type"].get<std::string>() : "";
    if (type == "error") {
        std::string message = "pairing endpoint returned an error";
        if (envelope.contains("error") && envelope["error"].is_object() &&
            envelope["error"].contains("message") && envelope["error"]["message"].is_string()) {
            message = envelope["error"]["message"].get<std::string>();
        }
        if (error) {
            *error = redact_text(message);
        }
        status->state = PairingState::Failed;
        status->message = error ? *error : message;
        status->terminal = true;
        return false;
    }
    if (type != "result" || !envelope.contains("data")) {
        if (error) {
            *error = "unexpected pairing response envelope";
        }
        status->state = PairingState::Failed;
        return false;
    }

    nlohmann::json result;
    if (envelope["data"].is_string()) {
        if (!decode_devalue_data(envelope["data"].get<std::string>(), &result, error)) {
            status->state = PairingState::Failed;
            return false;
        }
    } else {
        result = envelope["data"];
        if (result.is_object() && result.contains("_")) {
            result = result["_"];
        }
    }
    if (!result.is_object() || !result.contains("status") || !result["status"].is_string()) {
        if (error) {
            *error = "pairing response has no status";
        }
        status->state = PairingState::Failed;
        return false;
    }

    const std::string value = result["status"].get<std::string>();
    if (value == "pending") {
        status->state = PairingState::Waiting;
        status->message = "Waiting for scan";
    } else if (value == "scanned") {
        status->state = PairingState::Scanned;
        status->message = "Confirm on your device";
        if (result.contains("code") && result["code"].is_string()) {
            status->code = result["code"].get<std::string>();
        }
    } else if (value == "paired") {
        status->state = PairingState::Paired;
        status->message = "Paired";
    } else {
        status->state = PairingState::Failed;
        status->message = "Unexpected pairing status: " + value;
        status->terminal = true;
        if (error) {
            *error = status->message;
        }
        return false;
    }
    return true;
}

void PairingSession::publish_update_locked(const PairingStatus& status, bool stale) {
    PairingUpdate update;
    update.valid = true;
    update.stale = stale;
    update.status = status;
    update.generation = challenge_generation_;
    update.qr = qr_;
    update.pairing_url = pairing_url_;
    update.challenge_changed = challenge_generation_ != published_generation_;
    published_generation_ = challenge_generation_;
    update.can_retry = state_ == PairingState::Scanned || state_ == PairingState::Failed ||
                       state_ == PairingState::Expired || paired_unacknowledged_ ||
                       !last_poll_error_.empty();
    {
        std::lock_guard<std::mutex> lock(update_mutex_);
        latest_ = std::move(update);
    }
}

void PairingSession::discard_stale_session() {
    std::lock_guard<std::mutex> round(round_mutex_);
    discard_stale_session_locked();
}

void PairingSession::discard_stale_session_locked() {
    HttpRequest request;
    request.method = "DELETE";
    request.url = options_.origin + "/api/car/session";
    request.headers = {
        {"Origin", options_.origin},
        {"Referer", options_.origin + "/car"},
        {"User-Agent", kUserAgent},
    };
    std::string error;
    const HttpResponse response = http_.request(request, &error);
    if (!error.empty()) {
        log::warn("pairing: cannot discard the stale session: %s", redact_text(error).c_str());
    } else if (response.status >= 400 && response.status != 401) {
        log::warn("pairing: stale session discard returned HTTP %d", response.status);
    } else {
        log::info("pairing: discarded the stale paired session");
    }
    // The next round must not inherit the abandoned account even when the
    // server's 204 does not expire the cookie. This is local state, so do it
    // regardless of the DELETE response or its headers.
    http_.clear_cookies();
}

bool PairingSession::poll_once() {
    // The delete for an abandoned round and the generation/verifier this poll
    // is about to claim must be sampled together. Otherwise restart() can slip
    // between take_discard_request() and the generation read: the old session
    // is deleted with one round's state while the claim is sent for another.
    std::unique_lock<std::mutex> round(round_mutex_);

    bool discard = false;
    bool terminal_paired = false;
    bool terminal_stop = false;
    bool no_challenge = false;
    bool refresh_failed = false;
    PairingStatus refresh_failure;
    PairingStatus status;
    std::string verifier;
    int generation = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        discard = discard_pending_;
        discard_pending_ = false;

        if (state_ == PairingState::Paired) {
            // Keep the worker alive while a successful snapshot is waiting
            // for the UI. A restart at this point must be able to continue
            // with a new challenge instead of leaving the poller dead.
            terminal_paired = true;
        } else if (state_ == PairingState::Failed || state_ == PairingState::Expired) {
            terminal_stop = true;
        } else if (challenge_generation_ == 0) {
            no_challenge = true;
        } else {
            if (options_.challenge_lifetime_seconds > 0 && last_code_.empty() &&
                std::chrono::steady_clock::now() - challenge_created_ >=
                    std::chrono::seconds(options_.challenge_lifetime_seconds)) {
                std::string refresh_error;
                if (!regenerate_challenge_locked(&refresh_error)) {
                    last_poll_error_ = refresh_error;
                    refresh_failure.state = state_;
                    refresh_failure.message = refresh_error;
                    publish_update_locked(refresh_failure);
                    refresh_failed = true;
                } else {
                    log::info("pairing challenge refreshed");
                    PairingStatus refreshed;
                    refreshed.state = PairingState::Waiting;
                    refreshed.message = "Waiting for scan";
                    publish_update_locked(refreshed);
                }
            }
            if (!refresh_failed) {
                generation = challenge_generation_;
                verifier = verifier_;
                status.state = state_;
                status.code = last_code_;
            }
        }
    }

    if (discard) {
        discard_stale_session_locked();
    }
    if (no_challenge) {
        log::warn("pairing: poll requested before a challenge exists");
        return false;
    }
    if (refresh_failed) {
        return true;
    }
    if (terminal_paired || terminal_stop) {
        // restart() may have run while the DELETE was in flight. Keep the
        // worker alive for the new challenge rather than letting the stale
        // terminal snapshot stop it.
        bool revived = false;
        bool keep_alive = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            revived = state_ == PairingState::Waiting || state_ == PairingState::Scanned;
            keep_alive = terminal_paired && paired_unacknowledged_;
        }
        if (revived) {
            return true;
        }
        return terminal_paired ? keep_alive : false;
    }

    nlohmann::json devalue = nlohmann::json::array();
    devalue.push_back({{"device", 1}, {"verifier", 2}});
    devalue.push_back("car");
    devalue.push_back(verifier);
    const std::string devalue_text = devalue.dump();
    const std::string payload =
        base64_encode(reinterpret_cast<const std::uint8_t*>(devalue_text.data()), devalue_text.size());
    nlohmann::json request_body = {
        {"payload", payload},
        {"refreshes", nlohmann::json::array()},
    };

    std::string request_error;
    const HttpResponse response =
        post_remote("claimDevicePairing", request_body.dump(), &request_error);
    PairingStatus decoded;
    std::string decode_error;
    bool decoded_ok = false;
    if (request_error.empty()) {
        decoded_ok = decode_pairing_response(response.body, &decoded, &decode_error);
    }

    bool stale = false;
    bool discard_reply = false;
    bool stop_worker = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (generation != challenge_generation_ || state_ == PairingState::Expired) {
            stale = true;
            // The web deletes a session created by a round the user already
            // replaced, so a late confirmation cannot leave that account
            // attached to the car.
            discard_reply = decoded_ok && decoded.state == PairingState::Paired;
        } else if (!request_error.empty() || !decoded_ok) {
            const std::string message =
                !request_error.empty() ? request_error
                                       : (decode_error.empty() ? "pairing request failed" : decode_error);
            last_poll_error_ = message;
            PairingStatus failed;
            const bool transient_http =
                !request_error.empty() &&
                (response.status == 0 || response.status == 408 || response.status == 429 ||
                 response.status < 400 || response.status >= 500);
            const bool terminal_response =
                (!request_error.empty() && !transient_http) ||
                (request_error.empty() && decoded.terminal);
            if (!terminal_response) {
                // Transport, 5xx, 408/429 and payload errors are transient:
                // keep the current challenge and QR usable while retrying.
                failed.state = state_;
                failed.code = last_code_;
            } else {
                // The server rejected the challenge; stop polling until the
                // user retries with a fresh one.
                state_ = PairingState::Failed;
                last_code_.clear();
                failed.state = state_;
                failed.terminal = true;
                stop_worker = true;
            }
            failed.message = message;
            publish_update_locked(failed);
        } else {
            last_poll_error_.clear();
            state_ = decoded.state;
            if (decoded.state == PairingState::Waiting) {
                // The server explicitly sent the challenge back to pending;
                // drop the confirmation code so the QR is shown again.
                decoded.code.clear();
                last_code_.clear();
            } else if (decoded.state == PairingState::Scanned) {
                last_code_ = decoded.code;
            } else if (decoded.state == PairingState::Paired) {
                last_code_.clear();
                paired_generation_ = generation;
                paired_unacknowledged_ = true;
            }
            publish_update_locked(decoded);
        }
    }

    if (stale) {
        log::info("pairing: ignoring a reply from an abandoned challenge");
        if (discard_reply) {
            // poll_once() holds round_mutex_ across this claim, so use the
            // locked delete rather than taking it twice.
            discard_stale_session_locked();
        }
    }
    return !stop_worker;
}

bool PairingSession::take_discard_request() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const bool discard = discard_pending_;
    discard_pending_ = false;
    return discard;
}

void PairingSession::run_poll_loop() {
    const auto interval = std::chrono::seconds(std::max(1, options_.poll_interval_seconds));
    bool first = true;
    while (true) {
        bool stopped_with_discard = false;
        if (!first) {
            std::unique_lock<std::mutex> lock(poll_wait_mutex_);
            poll_wake_.wait_for(lock, interval, [this] { return poll_stop_ || poll_wakeup_; });
            if (poll_stop_) {
                stopped_with_discard = take_discard_request();
                poll_running_ = false;
                lock.unlock();
                // An unconsumed successful pairing can still own a session
                // even though the UI stopped before acknowledging it.
                if (stopped_with_discard) {
                    discard_stale_session();
                }
                return;
            }
            poll_wakeup_ = false;
        }
        first = false;
        if (poll_once()) {
            continue;
        }

        // A stop result is only final if no restart replaced the challenge
        // while poll_once() was publishing it. Holding poll_wait_mutex_ here
        // closes the race with restart()'s start_polling()/wake.
        bool stop = false;
        {
            std::lock_guard<std::mutex> poll_lock(poll_wait_mutex_);
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                stop = state_ == PairingState::Failed || state_ == PairingState::Expired ||
                       (state_ == PairingState::Paired && !paired_unacknowledged_);
            }
            if (stop) {
                poll_running_ = false;
            } else {
                poll_wakeup_ = true;
            }
        }
        if (stop) {
            return;
        }
    }
}

bool PairingSession::establish_session(std::string* error) {
    if (error) {
        error->clear();
    }
    HttpRequest request;
    request.method = "POST";
    request.url = options_.origin + "/api/car/session";
    request.headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Origin", options_.origin},
        {"Referer", options_.origin + "/car"},
        {"User-Agent", kUserAgent},
    };
    request.body = "{}";

    auto response = http_.request(request, error);
    if (error && !error->empty()) {
        return false;
    }
    if (response.status == 401) {
        if (error) {
            *error = "car session was rejected (401); pairing did not produce a session";
        }
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        if (error) {
            *error = "car session returned HTTP " + std::to_string(response.status);
        }
        return false;
    }
    return true;
}

bool PairingSession::load_car_session(CarSession* session, std::string* error) {
    if (error) {
        error->clear();
    }
    std::string page;
    if (!http_get(http_, options_.origin + "/car", &page, error)) {
        return false;
    }
    // Keep the page for remote-hash discovery if this session turns out to be
    // unusable; pairing start would otherwise fetch the same page again.
    cached_car_page_ = page;

    CarSession parsed;
    if (!parse_car_page(page, &parsed, error)) {
        return false;
    }
    *session = std::move(parsed);
    cached_car_page_.clear();
    return true;
}

bool PairingSession::refresh_authorization(CarSession* session, std::string* error) {
    if (error) {
        error->clear();
    }
    if (!session) {
        if (error) {
            *error = "car session output is null";
        }
        return false;
    }
    std::string page;
    if (!http_get(http_, options_.origin + "/car", &page, error)) {
        return false;
    }
    CarSession parsed;
    if (!parse_car_page(page, &parsed, error)) {
        return false;
    }
    if (parsed.thread_id != session->thread_id) {
        if (error) {
            *error = "the car page now belongs to another thread";
        }
        return false;
    }
    session->capability = std::move(parsed.capability);
    session->requires_sudo_for_write = parsed.requires_sudo_for_write;
    return true;
}

bool PairingSession::fetch_actor_token(CarSession* session, std::string* error) {
    if (error) {
        error->clear();
    }
    if (session->thread_id.empty()) {
        if (error) {
            *error = "thread id is empty";
        }
        return false;
    }
    const std::string amp_url =
        strip_trailing_slash(session->actor_amp_url.empty() ? options_.origin : session->actor_amp_url);
    // A missing capability is not write access. Request the least-privileged
    // token and leave the call gate closed until /car reports "write".
    const std::string capability = session->capability.empty() ? "read" : session->capability;

    HttpRequest request;
    request.method = "POST";
    request.url = amp_url + "/api/thread-actors?threadId=" + url_encode(session->thread_id) +
                  "&capability=" + url_encode(capability);
    request.headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Origin", origin_of(amp_url)},
        {"Referer", options_.origin + "/car"},
        {"User-Agent", kUserAgent},
    };
    request.body = nlohmann::json{{"threadId", session->thread_id}, {"capability", capability}}.dump();

    auto response = http_.request(request, error);
    if (error && !error->empty()) {
        return false;
    }
    if (response.status != 200 && response.status != 201) {
        if (error) {
            *error = "thread actor token returned HTTP " + std::to_string(response.status);
            if (!response.body.empty()) {
                *error += ": " + redact_text(response.body, 160);
            }
        }
        return false;
    }
    if (response.body.find("<!doctype html") != std::string::npos ||
        response.body.find("<html") != std::string::npos) {
        if (error) {
            *error = "thread actor token request was not authenticated (got the sign-in page)";
        }
        return false;
    }
    auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (json.is_discarded() || !json.contains("wsToken") || !json["wsToken"].is_string()) {
        if (error) {
            *error = "thread actor token response has no wsToken";
            if (!response.body.empty()) {
                *error += ": " + redact_text(response.body, 160);
            }
        }
        return false;
    }
    session->ws_token = json["wsToken"].get<std::string>();
    return true;
}

bool PairingSession::persist(const CarSession& session, std::string* error) {
    if (error) {
        error->clear();
    }
    if (!store_) {
        if (error) {
            *error = "secure store is unavailable";
        }
        return false;
    }
    const std::string cookies = http_.export_cookies();
    if (!cookies.empty() && !store_->save("http.cookies", cookies)) {
        if (error) {
            *error = "cannot save HTTP cookies";
        }
        return false;
    }
    nlohmann::json saved = {
        {"thread_id", session.thread_id},
        {"actor_base_url", session.actor_base_url},
        {"actor_amp_url", session.actor_amp_url},
        {"actor_pool", session.actor_pool},
        {"capability", session.capability},
        {"requires_sudo_for_write", session.requires_sudo_for_write},
        {"saved_at", static_cast<long long>(std::time(nullptr))},
    };
    if (!store_->save("car.session", saved.dump())) {
        if (error) {
            *error = "cannot save car session";
        }
        return false;
    }
    return true;
}

void PairingSession::leave_session(std::string* error) {
    if (error) {
        error->clear();
    }
    HttpRequest request;
    request.method = "DELETE";
    request.url = options_.origin + "/api/car/session";
    request.headers = {
        {"Origin", options_.origin},
        {"Referer", options_.origin + "/car"},
        {"User-Agent", kUserAgent},
    };
    std::string delete_error;
    const HttpResponse response = http_.request(request, &delete_error);
    if (!delete_error.empty()) {
        log::warn("car session leave: %s", redact_text(delete_error).c_str());
        if (error) {
            *error = delete_error;
        }
    } else if (response.status >= 400 && response.status != 401) {
        log::warn("car session leave returned HTTP %d", response.status);
        if (error) {
            *error = "car session leave returned HTTP " + std::to_string(response.status);
        }
    } else {
        log::info("car session leave: temporary session deleted");
    }

    // The remote DELETE can fail after the server already forgot the session.
    // Local cleanup is unconditional so the next launch cannot restore the
    // previous user's account.
    if (store_) {
        store_->erase("car.session");
        store_->erase("http.cookies");
    }
    http_.clear_cookies();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = PairingState::Idle;
        qr_ = QrCode{};
        pairing_url_.clear();
        last_code_.clear();
        last_poll_error_.clear();
    }
}

bool build_actor_endpoint(const CarSession& session, const std::string& page_load_id,
                          ActorEndpoint* endpoint, std::string* error) {
    if (error) {
        error->clear();
    }
    if (!endpoint) {
        if (error) {
            *error = "actor endpoint output is null";
        }
        return false;
    }
    if (session.actor_base_url.empty()) {
        if (error) {
            *error = "actor base URL is empty";
        }
        return false;
    }
    if (session.thread_id.empty()) {
        if (error) {
            *error = "thread id is empty";
        }
        return false;
    }

    const std::regex url_re(R"(^(https?)://(?:([^:@/]+)(?::([^@/]*))?@)?([^/]+)(/[^?#]*)?)",
                            std::regex::icase);
    std::smatch match;
    if (!std::regex_match(session.actor_base_url, match, url_re)) {
        if (error) {
            *error = "invalid actor base URL";
        }
        return false;
    }
    const std::string scheme = match[1].str();
    const std::string user = match[2].matched ? match[2].str() : "";
    const std::string password = match[3].matched ? match[3].str() : "";
    const std::string host = match[4].str();
    std::string path = match[5].matched ? match[5].str() : "";
    path = strip_trailing_slash(path);

    const std::string namespace_name = user.empty() ? "default" : url_decode(user);
    const std::string token = url_decode(password);
    std::string scheme_lower = scheme;
    std::transform(scheme_lower.begin(), scheme_lower.end(), scheme_lower.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    const std::string ws_scheme = scheme_lower == "https" ? "wss" : "ws";
    const std::string gateway = ws_scheme + "://" + host + path + "/gateway/threadActor/websocket/";

    std::string query = "amp-browser-page-load-id=" + url_encode(page_load_id);
    query += "&rvt-namespace=" + url_encode(namespace_name);
    query += "&rvt-method=get";
    query += "&rvt-key=" + url_encode(session.thread_id);
    if (!token.empty()) {
        query += "&rvt-token=" + url_encode(token);
    }
    query += "&rvt-skip-ready-wait=true";

    endpoint->url = gateway + "?" + query;
    endpoint->display_url = gateway;
    endpoint->subprotocols.clear();
    endpoint->subprotocols.push_back("rivet");
    endpoint->subprotocols.push_back("rivet_encoding.bare");
    endpoint->subprotocols.push_back("rivet_skip_ready_wait");

    nlohmann::json connection = {
        {"browserPageLoadID", page_load_id},
        {"transport", "json-rpc"},
    };
    if (!session.ws_token.empty()) {
        connection["wsToken"] = session.ws_token;
    }
    endpoint->subprotocols.push_back("rivet_conn_params." + url_encode(connection.dump()));
    return true;
}

bool wait_actor_ready(HttpClient& http, const CarSession& session, const std::string& page_load_id,
                      std::string* error) {
    if (error) {
        error->clear();
    }
    ActorEndpoint endpoint;
    if (!build_actor_endpoint(session, page_load_id, &endpoint, error)) {
        return false;
    }

    std::string url = endpoint.url;
    if (url.rfind("wss://", 0) == 0) {
        url = "https://" + url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        url = "http://" + url.substr(5);
    }
    const std::string ws_path = "/websocket/?";
    const auto path_pos = url.find(ws_path);
    if (path_pos == std::string::npos) {
        if (error) {
            *error = "unexpected actor gateway path";
        }
        return false;
    }
    url.replace(path_pos, ws_path.size(), "/connect-ready-check?");
    const std::string skip_param = "&rvt-skip-ready-wait=true";
    const auto skip_pos = url.find(skip_param);
    if (skip_pos != std::string::npos) {
        url.erase(skip_pos, skip_param.size());
    }

    nlohmann::json connection = {
        {"browserPageLoadID", page_load_id},
        {"transport", "json-rpc"},
    };
    if (!session.ws_token.empty()) {
        connection["wsToken"] = session.ws_token;
    }

    HttpRequest request;
    request.url = url;
    request.headers = {
        {"Authorization", session.ws_token},
        {"x-rivet-conn-params", connection.dump()},
        {"Accept", "*/*"},
        {"User-Agent", kUserAgent},
    };

    for (int attempt = 0; attempt < 5; ++attempt) {
        std::string request_error;
        const HttpResponse response = http.request(request, &request_error);
        bool rivet_error = false;
        for (const HttpHeader& header : response.headers) {
            std::string name = header.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (name == "x-rivet-error") {
                rivet_error = true;
            }
        }
        if (response.status > 0 && response.status < 500 && !rivet_error) {
            return true;
        }
        if (error) {
            *error = "actor ready check returned HTTP " + std::to_string(response.status);
            if (!request_error.empty()) {
                *error += ": " + request_error;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

double ws_token_exp_seconds(const std::string& token) {
    const auto first = token.find('.');
    const auto second = token.find('.', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return 0.0;
    }
    std::string payload = token.substr(first + 1, second - first - 1);
    for (char& character : payload) {
        if (character == '-') {
            character = '+';
        } else if (character == '_') {
            character = '/';
        }
    }
    while (payload.size() % 4 != 0) {
        payload.push_back('=');
    }
    std::vector<unsigned char> decoded(payload.size());
    const int length = EVP_DecodeBlock(decoded.data(),
                                       reinterpret_cast<const unsigned char*>(payload.data()),
                                       static_cast<int>(payload.size()));
    if (length <= 0) {
        return 0.0;
    }
    const auto json = nlohmann::json::parse(decoded.begin(), decoded.begin() + length, nullptr, false);
    if (json.is_discarded() || !json.contains("exp") || !json["exp"].is_number()) {
        return 0.0;
    }
    return json["exp"].get<double>();
}

std::string redact_url(const std::string& url) {
    const std::regex re(
        R"(^([A-Za-z][A-Za-z0-9+.-]*://)(?:[^@/?#]*@)?([^/?#]+)([^?#]*)(?:[?#].*)?$)");
    std::smatch match;
    if (std::regex_match(url, match, re)) {
        return match[1].str() + match[2].str() + match[3].str();
    }
    return url;
}

std::string redact_text(const std::string& text, std::size_t max_length) {
    std::string redacted = text;
    redacted = std::regex_replace(redacted, std::regex(R"(pk_[A-Za-z0-9]+)"), "pk_[redacted]");
    redacted = std::regex_replace(redacted, std::regex(R"((Bearer\s+)[A-Za-z0-9._~+/=-]+)",
                                                       std::regex::icase),
                                  "$1[redacted]");
    redacted = std::regex_replace(redacted, std::regex(R"((rvt-token=)[^&"'\s]+)"), "$1[redacted]");
    redacted = std::regex_replace(redacted, std::regex(R"(("wsToken"\s*:\s*")[^"]*("))"),
                                  "$1[redacted]$2");
    redacted = std::regex_replace(redacted, std::regex(R"(("token"\s*:\s*")[^"]*("))"),
                                  "$1[redacted]$2");
    redacted = std::regex_replace(redacted, std::regex(R"(((?:https?|wss?)://)[^/\s"'@]+@)"),
                                  "$1[redacted]@");
    if (redacted.size() > max_length) {
        redacted.resize(max_length);
        redacted += "...";
    }
    return redacted;
}

std::string redact_json(const std::string& json, std::size_t max_length) {
    auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded()) {
        return redact_text(json, max_length);
    }
    redact_json_value(parsed);
    return redact_text(parsed.dump(), max_length);
}

}  // namespace pk
