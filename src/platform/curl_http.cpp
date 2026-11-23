#include "platform/curl_http.h"

#include <curl/curl.h>

#include <cctype>
#include <mutex>
#include <sstream>

namespace pk {
namespace {

struct TransferState {
    HttpResponse* response = nullptr;
    std::string header_name;
    std::string header_value;
};

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* state = static_cast<TransferState*>(userdata);
    state->response->body.append(data, size * count);
    return size * count;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::size_t write_header(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* state = static_cast<TransferState*>(userdata);
    const std::string line(data, size * count);
    if (line == "\r\n" || line == "\n") {
        return size * count;
    }
    if (line.rfind("HTTP/", 0) == 0) {
        // A redirect or a 100-continue response starts a new header block.
        state->response->headers.clear();
        return size * count;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return size * count;
    }
    state->response->headers.push_back(
        HttpHeader{trim(line.substr(0, colon)), trim(line.substr(colon + 1))});
    return size * count;
}

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

}  // namespace

CurlHttpClient::CurlHttpClient() {
    global_init();
    handle_ = curl_easy_init();
}

CurlHttpClient::~CurlHttpClient() {
    if (handle_) {
        curl_easy_cleanup(handle_);
        handle_ = nullptr;
    }
}

HttpResponse CurlHttpClient::request(const HttpRequest& request, std::string* error) {
    HttpResponse response;
    if (!handle_) {
        if (error) {
            *error = "libcurl is not initialized";
        }
        return response;
    }

    curl_easy_reset(handle_);
    curl_easy_setopt(handle_, CURLOPT_COOKIEFILE, "");

    TransferState state;
    state.response = &response;

    curl_easy_setopt(handle_, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle_, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_slist* header_list = nullptr;
    bool has_user_agent = false;
    bool has_expect = false;
    for (const auto& header : request.headers) {
        if (header.name.empty()) {
            continue;
        }
        if (header.name == "User-Agent") {
            has_user_agent = true;
        }
        if (header.name == "Expect") {
            has_expect = true;
        }
        header_list = curl_slist_append(header_list, (header.name + ": " + header.value).c_str());
    }
    if (!has_user_agent) {
        header_list = curl_slist_append(header_list, "User-Agent: puck-kiosk/0.1");
    }
    if (!has_expect) {
        header_list = curl_slist_append(header_list, "Expect:");
    }
    if (header_list) {
        curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, header_list);
    }

    if (request.method == "GET" && request.body.empty()) {
        curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
    } else if (request.method == "POST") {
        curl_easy_setopt(handle_, CURLOPT_POST, 1L);
        curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else {
        curl_easy_setopt(handle_, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty()) {
            curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
    }

    const CURLcode result = curl_easy_perform(handle_);
    long status = 0;
    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);

    if (header_list) {
        curl_slist_free_all(header_list);
    }

    if (result != CURLE_OK && error) {
        *error = std::string("HTTP request failed: ") + curl_easy_strerror(result);
    }
    return response;
}

std::string CurlHttpClient::export_cookies() const {
    std::string cookies;
    if (!handle_) {
        return cookies;
    }
    struct curl_slist* list = nullptr;
    if (curl_easy_getinfo(handle_, CURLINFO_COOKIELIST, &list) != CURLE_OK || !list) {
        return cookies;
    }
    for (struct curl_slist* node = list; node; node = node->next) {
        if (!node->data || node->data[0] == '\0') {
            continue;
        }
        cookies.append(node->data);
        cookies.push_back('\n');
    }
    curl_slist_free_all(list);
    return cookies;
}

void CurlHttpClient::import_cookies(const std::string& cookies) {
    if (!handle_) {
        return;
    }
    std::istringstream lines(cookies);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        // Netscape cookie files use comment lines, but curl also prefixes
        // HttpOnly cookies with "#HttpOnly_" and those are real cookies.
        if (line[0] == '#' && line.rfind("#HttpOnly_", 0) != 0) {
            continue;
        }
        curl_easy_setopt(handle_, CURLOPT_COOKIELIST, line.c_str());
    }
}

}  // namespace pk
