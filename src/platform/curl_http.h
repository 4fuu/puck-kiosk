#pragma once

#include "platform/http.h"

#include <string>

typedef void CURL;
typedef void CURLSH;

namespace pk {

// libcurl-backed HttpClient with an in-memory cookie jar. A single instance is
// used from one thread at a time; requests on one handle reuse its connections.
// request_many() overlaps independent transfers on additional handles that
// share this instance's connection cache, DNS cache, TLS session cache, and
// cookie jar.
class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;

    HttpResponse request(const HttpRequest& request, std::string* error) override;
    std::vector<HttpBatchResponse> request_many(
        const std::vector<HttpRequest>& requests) override;
    std::string export_cookies() const override;
    void import_cookies(const std::string& cookies) override;

    bool valid() const { return handle_ != nullptr; }

private:
    CURL* handle_ = nullptr;
    CURLSH* share_ = nullptr;
};

}  // namespace pk
