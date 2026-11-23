#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse request(const HttpRequest& request, std::string* error) = 0;
};

// A minimal WebSocket seam. The actor protocol is JSON-RPC over this socket.
class WebSocket {
public:
    virtual ~WebSocket() = default;
    virtual bool send_text(const std::string& text) = 0;
    virtual bool poll(std::string& message, std::string* error) = 0;
    virtual void close() = 0;
};

class WebSocketFactory {
public:
    virtual ~WebSocketFactory() = default;
    virtual std::unique_ptr<WebSocket> connect(const std::string& url,
                                               const std::vector<HttpHeader>& headers,
                                               std::string* error) = 0;
};

}  // namespace pk
