#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pk {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    std::vector<HttpHeader> headers;
    std::string body;
};

struct HttpBatchResponse {
    HttpResponse response;
    std::string error;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse request(const HttpRequest& request, std::string* error) = 0;

    // Fetches independent requests, possibly concurrently, and returns results
    // in request order. The default implementation is sequential, so a client
    // that only implements request() keeps working. Backends that share a
    // connection cache may overlap the transfers.
    virtual std::vector<HttpBatchResponse> request_many(
        const std::vector<HttpRequest>& requests) {
        std::vector<HttpBatchResponse> results;
        results.reserve(requests.size());
        for (const HttpRequest& request : requests) {
            HttpBatchResponse result;
            result.response = this->request(request, &result.error);
            results.push_back(std::move(result));
        }
        return results;
    }

    // Cookie state stays opaque to callers. The app persists this string in a
    // SecureStore and imports it on the next run; it is never logged.
    virtual std::string export_cookies() const { return {}; }
    virtual void import_cookies(const std::string& cookies) { (void)cookies; }
    // Erases the in-memory jar. import_cookies("") is not a clear: curl treats
    // an empty line as no-op, so shared-device cleanup needs this explicit
    // operation.
    virtual void clear_cookies() {}
};

// A minimal WebSocket seam. The actor protocol is JSON-RPC over this socket.
class WebSocket {
public:
    virtual ~WebSocket() = default;
    virtual bool send_text(const std::string& text) = 0;

    // Waits up to about a second for a text frame. Returns false on timeout or
    // close; is_open() distinguishes the two.
    virtual bool poll(std::string& message, std::string* error) = 0;
    virtual bool is_open() const { return true; }
    virtual std::string close_reason() const { return {}; }
    virtual void close() = 0;
};

class WebSocketFactory {
public:
    virtual ~WebSocketFactory() = default;
    virtual std::unique_ptr<WebSocket> connect(const std::string& url,
                                               const std::vector<HttpHeader>& headers,
                                               std::string* error) = 0;

    // The actor gateway negotiates its transport through WebSocket
    // subprotocols, so backends that can send them override this overload.
    virtual std::unique_ptr<WebSocket> connect(const std::string& url,
                                               const std::vector<HttpHeader>& headers,
                                               const std::vector<std::string>& subprotocols,
                                               std::string* error) {
        (void)subprotocols;
        return connect(url, headers, error);
    }
};

}  // namespace pk
