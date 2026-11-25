#pragma once

#include "app/pairing.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pk {

struct SessionStartupStatus {
    bool finished = false;
    bool resumed = false;
    bool failed = false;
    std::string phase = "Starting up";
    std::string error;
};

// Runs the network-bound part of startup off the UI thread: cookie restore,
// validating a saved car session against /car, remote-hash discovery, and
// challenge generation. The app renders a loading state until finished() is
// true, then calls wait() and takes over pairing().
class SessionStartup {
public:
    SessionStartup(HttpClient& http, SecureStore* store, PairingOptions options, bool force_pair);
    ~SessionStartup();

    SessionStartup(const SessionStartup&) = delete;
    SessionStartup& operator=(const SessionStartup&) = delete;

    void start();
    void wait();
    SessionStartupStatus status() const;

    // Valid after wait(): whether the saved session was reused and, if so, its
    // car data. pairing() is only touched by the UI thread after wait().
    bool resumed() const { return resumed_; }
    CarSession session() const;
    PairingSession& pairing() { return pairing_; }

private:
    void run();

    PairingSession pairing_;
    bool force_pair_ = false;
    std::thread thread_;
    mutable std::mutex mutex_;
    SessionStartupStatus status_;
    CarSession session_;
    bool resumed_ = false;
    bool started_ = false;
};

// The web re-reads /car every 10 seconds while the actor is read-only, so a
// passkey confirmation can unlock calling without restarting the kiosk. The
// request runs on a worker; the UI applies the latest capability snapshot.
struct AuthorizationStatus {
    bool valid = false;
    bool changed = false;
    CarSession session;
    std::string error;
};

class AuthorizationWatcher {
public:
    AuthorizationWatcher(PairingSession& pairing,
                         std::chrono::milliseconds interval = std::chrono::seconds(10));
    ~AuthorizationWatcher();

    AuthorizationWatcher(const AuthorizationWatcher&) = delete;
    AuthorizationWatcher& operator=(const AuthorizationWatcher&) = delete;

    void start(const CarSession& session);
    void stop();
    bool take(AuthorizationStatus* status);

private:
    void run();

    PairingSession& pairing_;
    std::chrono::milliseconds interval_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    CarSession session_;
    AuthorizationStatus pending_;
    bool active_ = false;
    bool stop_ = false;
};

bool explicit_write_permission(const CarSession& session);

// Shared gate for the on-screen call button and --call. An explicit request
// waits until the session has explicit write permission and the actor socket is
// connected; the gate then releases exactly one start attempt.
class CallGate {
public:
    bool update(bool can_write, bool actor_connected);
    bool request();
    bool can_call() const { return can_write_ && actor_connected_; }
    bool pending() const { return requested_; }
    void reset();

private:
    bool consume();

    bool can_write_ = false;
    bool actor_connected_ = false;
    bool requested_ = false;
};

// Tracks whether the *current* actor socket was opened with write capability.
// A /car capability change is not enough: refresh the token, replace the
// socket, and only then report write_ready(). Failures stay visible and retry
// the same pending call request instead of falling back to the old socket.
class WriteAccessCoordinator {
public:
    using TokenRefresh = std::function<bool(std::string* error)>;
    using ActorConnect = std::function<bool(std::string* error)>;

    WriteAccessCoordinator(TokenRefresh refresh_token, ActorConnect connect_actor,
                           std::chrono::milliseconds retry_interval = std::chrono::seconds(5));

    void set_write(bool write);
    // The initial socket was opened with a fresh write token already.
    void mark_ready();
    void tick(std::chrono::steady_clock::time_point now);

    bool write_ready() const { return write_ready_; }
    bool waiting() const { return waiting_; }
    const std::string& message() const { return message_; }
    int attempts() const { return attempts_; }

private:
    TokenRefresh refresh_token_;
    ActorConnect connect_actor_;
    std::chrono::milliseconds retry_interval_;
    bool write_ = false;
    bool write_ready_ = false;
    bool waiting_ = false;
    bool needs_attempt_ = false;
    int attempts_ = 0;
    std::string message_;
    std::chrono::steady_clock::time_point next_attempt_{};
};

}  // namespace pk
