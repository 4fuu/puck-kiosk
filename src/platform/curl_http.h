#pragma once

#include "platform/http.h"

#include <string>

typedef void CURL;

namespace pk {

// libcurl-backed HttpClient with an in-memory cookie jar. Requests are
// sequential, which matches the single-threaded kiosk UI.
class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;

    HttpResponse request(const HttpRequest& request, std::string* error) override;
    std::string export_cookies() const override;
    void import_cookies(const std::string& cookies) override;

    bool valid() const { return handle_ != nullptr; }

private:
    CURL* handle_ = nullptr;
};

}  // namespace pk
