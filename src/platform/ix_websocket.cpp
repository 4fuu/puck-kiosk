#include "platform/ix_websocket.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace pk {

struct IxWebSocketState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> messages;
    bool open = false;
    bool closed = false;
    std::string error;
    int close_code = 0;
    std::string close_reason;
};

IxWebSocket::IxWebSocket(std::shared_ptr<ix::WebSocket> socket,
                         std::shared_ptr<IxWebSocketState> state)
    : socket_(std::move(socket)), state_(std::move(state)) {}

IxWebSocket::~IxWebSocket() {
    close();
}

bool IxWebSocket::send_text(const std::string& text) {
    if (!socket_ || socket_->getReadyState() != ix::ReadyState::Open) {
        return false;
    }
    return socket_->sendText(text).success;
}

bool IxWebSocket::poll(std::string& message, std::string* error) {
    if (!state_) {
        return false;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->cv.wait_for(lock, std::chrono::seconds(1), [this] {
        return !state_->messages.empty() || state_->closed;
    });
    if (!state_->messages.empty()) {
        message = std::move(state_->messages.front());
        state_->messages.pop_front();
        return true;
    }
    if (!state_->error.empty() && error) {
        *error = state_->error;
    }
    return false;
}

bool IxWebSocket::is_open() const {
    return socket_ && socket_->getReadyState() == ix::ReadyState::Open;
}

std::string IxWebSocket::close_reason() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->close_reason.empty()) {
        return {};
    }
    return "code " + std::to_string(state_->close_code) + ": " + state_->close_reason;
}

void IxWebSocket::close() {
    if (socket_) {
        socket_->stop();
    }
}

IxWebSocketFactory::IxWebSocketFactory() {
    static std::once_flag once;
    std::call_once(once, [] { ix::initNetSystem(); });
}

IxWebSocketFactory::~IxWebSocketFactory() = default;

std::unique_ptr<WebSocket> IxWebSocketFactory::connect(const std::string& url,
                                                       const std::vector<HttpHeader>& headers,
                                                       std::string* error) {
    return connect(url, headers, {}, error);
}

std::unique_ptr<WebSocket> IxWebSocketFactory::connect(
    const std::string& url,
    const std::vector<HttpHeader>& headers,
    const std::vector<std::string>& subprotocols,
    std::string* error) {
    auto state = std::make_shared<IxWebSocketState>();
    auto socket = std::make_shared<ix::WebSocket>();
    socket->setUrl(url);
    socket->setHandshakeTimeout(15);
    socket->setPingInterval(30);
    socket->disableAutomaticReconnection();

    for (const auto& protocol : subprotocols) {
        socket->addSubProtocol(protocol);
    }

    ix::WebSocketHttpHeaders extra_headers;
    for (const auto& header : headers) {
        if (!header.name.empty()) {
            extra_headers[header.name] = header.value;
        }
    }
    socket->setExtraHeaders(extra_headers);

    socket->setOnMessageCallback([state](const ix::WebSocketMessagePtr& message) {
        std::lock_guard<std::mutex> lock(state->mutex);
        switch (message->type) {
            case ix::WebSocketMessageType::Open:
                state->open = true;
                state->cv.notify_all();
                break;
            case ix::WebSocketMessageType::Message:
                // RivetKit's "bare" encoding delivers JSON-RPC as binary frames.
                state->messages.push_back(message->str);
                state->cv.notify_all();
                break;
            case ix::WebSocketMessageType::Close:
                state->closed = true;
                state->close_code = message->closeInfo.code;
                state->close_reason = message->closeInfo.reason;
                if (!state->open) {
                    state->error = "WebSocket closed during connect (code " +
                                   std::to_string(message->closeInfo.code) + ")";
                }
                state->cv.notify_all();
                break;
            case ix::WebSocketMessageType::Error: {
                state->closed = true;
                std::string reason = message->errorInfo.reason;
                if (reason.empty()) {
                    reason = "WebSocket connection error";
                }
                if (message->errorInfo.http_status != 0) {
                    reason += " (HTTP " + std::to_string(message->errorInfo.http_status) + ")";
                }
                state->error = reason;
                state->cv.notify_all();
                break;
            }
            default:
                break;
        }
    });

    socket->start();

    bool opened = false;
    std::string failure;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::seconds(16),
                           [&state] { return state->open || state->closed; });
        opened = state->open;
        failure = state->error;
    }

    if (!opened) {
        socket->stop();
        if (error) {
            *error = failure.empty() ? "WebSocket handshake timed out" : failure;
        }
        return nullptr;
    }

    return std::make_unique<IxWebSocket>(std::move(socket), std::move(state));
}

}  // namespace pk
