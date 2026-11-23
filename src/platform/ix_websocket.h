#pragma once

#include "platform/http.h"

#include <memory>

namespace ix {
class WebSocket;
}  // namespace ix

namespace pk {

struct IxWebSocketState;

class IxWebSocket final : public WebSocket {
public:
    IxWebSocket(std::shared_ptr<ix::WebSocket> socket, std::shared_ptr<IxWebSocketState> state);
    ~IxWebSocket() override;

    bool send_text(const std::string& text) override;
    bool poll(std::string& message, std::string* error) override;
    bool is_open() const override;
    std::string close_reason() const override;
    void close() override;

private:
    std::shared_ptr<ix::WebSocket> socket_;
    std::shared_ptr<IxWebSocketState> state_;
};

class IxWebSocketFactory final : public WebSocketFactory {
public:
    IxWebSocketFactory();
    ~IxWebSocketFactory() override;

    std::unique_ptr<WebSocket> connect(const std::string& url,
                                       const std::vector<HttpHeader>& headers,
                                       std::string* error) override;
    std::unique_ptr<WebSocket> connect(const std::string& url,
                                       const std::vector<HttpHeader>& headers,
                                       const std::vector<std::string>& subprotocols,
                                       std::string* error) override;
};

}  // namespace pk
