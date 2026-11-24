#pragma once

#include "app/pairing.h"

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

}  // namespace pk
