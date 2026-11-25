#include "platform/curl_http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace pk {
namespace {

// Browsers open about six connections per host; more would only queue at the
// origin. Batches from PairingSession use the same limit.
constexpr std::size_t kMaxParallelRequests = 6;

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

void configure_handle(CURL* handle, CURLSH* share, const HttpRequest& request,
                      TransferState* state, curl_slist** header_list) {
    curl_easy_reset(handle);
    // curl_easy_reset keeps the live connections but drops the share, so put it
    // back before the transfer starts.
    if (share) {
        curl_easy_setopt(handle, CURLOPT_SHARE, share);
    }
    curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, state);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, state);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);

    bool has_user_agent = false;
    bool has_expect = false;
    bool has_accept_encoding = false;
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
        if (header.name == "Accept-Encoding") {
            has_accept_encoding = true;
        }
        *header_list =
            curl_slist_append(*header_list, (header.name + ": " + header.value).c_str());
    }
    // Asset downloads ask for an identity encoding because the Windows curl
    // build does not decode every encoding the CDN can negotiate.
    if (!has_accept_encoding) {
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    }
    if (!has_user_agent) {
        *header_list = curl_slist_append(*header_list, "User-Agent: puck-kiosk/0.1");
    }
    if (!has_expect) {
        *header_list = curl_slist_append(*header_list, "Expect:");
    }
    if (*header_list) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, *header_list);
    }

    if (request.method == "GET" && request.body.empty()) {
        curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    } else if (request.method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty()) {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(request.body.size()));
        }
    }
}

}  // namespace

CurlHttpClient::CurlHttpClient() {
    global_init();
    handle_ = curl_easy_init();
    share_ = curl_share_init();
    if (share_) {
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    }
}

CurlHttpClient::~CurlHttpClient() {
    if (handle_) {
        curl_easy_cleanup(handle_);
        handle_ = nullptr;
    }
    if (share_) {
        curl_share_cleanup(share_);
        share_ = nullptr;
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

    TransferState state;
    state.response = &response;
    curl_slist* header_list = nullptr;
    configure_handle(handle_, share_, request, &state, &header_list);

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

std::vector<HttpBatchResponse> CurlHttpClient::request_many(
    const std::vector<HttpRequest>& requests) {
    std::vector<HttpBatchResponse> results(requests.size());
    if (requests.empty()) {
        return results;
    }
    if (!handle_) {
        for (HttpBatchResponse& result : results) {
            result.error = "libcurl is not initialized";
        }
        return results;
    }
    if (requests.size() == 1) {
        results[0].response = request(requests[0], &results[0].error);
        return results;
    }

    CURLM* multi = curl_multi_init();
    if (!multi) {
        for (std::size_t i = 0; i < requests.size(); ++i) {
            results[i].response = request(requests[i], &results[i].error);
        }
        return results;
    }

    struct Pending {
        CURL* easy = nullptr;
        TransferState state;
        curl_slist* headers = nullptr;
        std::size_t index = 0;
    };
    std::vector<std::unique_ptr<Pending>> pending;
    std::size_t next = 0;

    auto add_next = [&]() {
        auto item = std::make_unique<Pending>();
        item->index = next;
        item->state.response = &results[next].response;
        item->easy = curl_easy_init();
        if (!item->easy) {
            results[next].error = "libcurl could not create a request handle";
            ++next;
            return;
        }
        configure_handle(item->easy, share_, requests[next], &item->state, &item->headers);
        if (curl_multi_add_handle(multi, item->easy) != CURLM_OK) {
            results[next].error = "libcurl could not queue the request";
            curl_easy_cleanup(item->easy);
            if (item->headers) {
                curl_slist_free_all(item->headers);
            }
            ++next;
            return;
        }
        ++next;
        pending.push_back(std::move(item));
    };

    auto finish = [&](CURL* easy, CURLcode code) {
        const auto found = std::find_if(
            pending.begin(), pending.end(),
            [easy](const std::unique_ptr<Pending>& item) { return item->easy == easy; });
        if (found == pending.end()) {
            return;
        }
        Pending& item = **found;
        long status = 0;
        curl_easy_getinfo(item.easy, CURLINFO_RESPONSE_CODE, &status);
        results[item.index].response.status = static_cast<int>(status);
        if (code != CURLE_OK) {
            results[item.index].error =
                std::string("HTTP request failed: ") + curl_easy_strerror(code);
        }
        curl_multi_remove_handle(multi, item.easy);
        curl_easy_cleanup(item.easy);
        if (item.headers) {
            curl_slist_free_all(item.headers);
        }
        pending.erase(found);
    };

    while (next < requests.size() && pending.size() < kMaxParallelRequests) {
        add_next();
    }

    int running = 0;
    curl_multi_perform(multi, &running);
    for (;;) {
        CURLMsg* message = nullptr;
        int queued = 0;
        while ((message = curl_multi_info_read(multi, &queued)) != nullptr) {
            if (message->msg == CURLMSG_DONE) {
                finish(message->easy_handle, message->data.result);
            }
        }
        if (pending.empty() && next >= requests.size()) {
            break;
        }
        while (next < requests.size() && pending.size() < kMaxParallelRequests) {
            add_next();
        }
        if (pending.empty()) {
            break;
        }
        curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        curl_multi_perform(multi, &running);
    }

    curl_multi_cleanup(multi);
    return results;
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

void CurlHttpClient::clear_cookies() {
    if (!handle_) {
        return;
    }
    // "ALL" is curl's documented erase-all command. An empty import_cookies
    // string iterates zero lines and leaves the jar untouched.
    curl_easy_setopt(handle_, CURLOPT_COOKIELIST, "ALL");
}

}  // namespace pk
