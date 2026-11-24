#include "app/session_startup.h"

#include "core/log.h"

#include <chrono>

namespace pk {
namespace {

using Clock = std::chrono::steady_clock;

long long milliseconds_since(Clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
}

}  // namespace

SessionStartup::SessionStartup(HttpClient& http, SecureStore* store, PairingOptions options,
                               bool force_pair)
    : pairing_(http, store, std::move(options)), force_pair_(force_pair) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.phase = "Restoring session";
}

SessionStartup::~SessionStartup() {
    wait();
}

void SessionStartup::start() {
    if (started_) {
        return;
    }
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void SessionStartup::wait() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

SessionStartupStatus SessionStartup::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

CarSession SessionStartup::session() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_;
}

void SessionStartup::run() {
    const auto started = Clock::now();
    std::string error;

    CarSession saved;
    if (pairing_.restore(&saved, &error)) {
        if (!saved.thread_id.empty()) {
            log::info("restored saved session for thread %s", saved.thread_id.c_str());
        } else {
            log::info("restored saved cookies");
        }
    } else {
        log::info("%s", error.c_str());
    }

    if (!force_pair_ && !saved.thread_id.empty()) {
        CarSession session;
        if (pairing_.load_car_session(&session, &error) && !session.thread_id.empty()) {
            log::info("saved session still valid; skipping pairing");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                session_ = std::move(session);
                resumed_ = true;
                status_.resumed = true;
                status_.phase = "Connecting";
                status_.finished = true;
            }
            log::info("startup: saved session restored after %lld ms",
                      milliseconds_since(started));
            return;
        }
        log::info("saved session not reusable (%s); pairing again", redact_text(error).c_str());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = "Preparing the QR code";
    }
    if (!pairing_.start(&error)) {
        log::error("cannot start pairing: %s", error.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.failed = true;
            status_.phase = "Pairing unavailable";
            status_.error = error;
            status_.finished = true;
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = "Waiting for scan";
        status_.finished = true;
    }
    log::info("startup: pairing ready after %lld ms", milliseconds_since(started));
}

}  // namespace pk
