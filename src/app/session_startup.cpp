#include "app/session_startup.h"

#include "core/log.h"

#include <algorithm>
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
    const auto restore_started = Clock::now();
    if (force_pair_) {
        // --pair is an explicit request for a new account; do not import the
        // previous user's session or cookie jar.
        log::info("forced pairing: ignoring the saved session and cookies");
    } else if (pairing_.restore(&saved, &error)) {
        if (!saved.thread_id.empty()) {
            log::info("restored saved session for thread %s", saved.thread_id.c_str());
        } else {
            log::info("restored saved cookies");
        }
    } else {
        log::info("%s", error.c_str());
    }
    log::info("startup: restore %lld ms", milliseconds_since(restore_started));

    if (!force_pair_ && !saved.thread_id.empty()) {
        CarSession session;
        const auto validation_started = Clock::now();
        const bool valid =
            pairing_.load_car_session(&session, &error) && !session.thread_id.empty();
        log::info("startup: session validation %lld ms", milliseconds_since(validation_started));
        if (valid) {
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

    // Begin claiming the challenge before the UI leaves its loading state.
    // Asset downloads use a different client, so a slow fetch cannot delay
    // this poll or the scan confirmation.
    pairing_.start_polling();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = "Waiting for scan";
        status_.finished = true;
    }
    log::info("startup: pairing ready after %lld ms", milliseconds_since(started));
}

bool explicit_write_permission(const CarSession& session) {
    return session.capability == "write";
}

AuthorizationWatcher::AuthorizationWatcher(PairingSession& pairing,
                                           std::chrono::milliseconds interval)
    : pairing_(pairing), interval_(interval) {}

AuthorizationWatcher::~AuthorizationWatcher() {
    stop();
}

void AuthorizationWatcher::start(const CarSession& session) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = session;
        active_ = !explicit_write_permission(session_);
        stop_ = false;
    }
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { run(); });
    }
    wake_.notify_all();
}

void AuthorizationWatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool AuthorizationWatcher::take(AuthorizationStatus* status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.valid) {
        return false;
    }
    if (status) {
        *status = pending_;
    }
    pending_ = AuthorizationStatus{};
    return true;
}

void AuthorizationWatcher::run() {
    while (true) {
        CarSession current;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, interval_, [this] { return stop_; });
            if (stop_) {
                return;
            }
            if (!active_ || explicit_write_permission(session_)) {
                continue;
            }
            current = session_;
        }

        CarSession next = current;
        std::string error;
        const bool ok = pairing_.refresh_authorization(&next, &error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            pending_.valid = true;
            pending_.session = session_;
            if (ok) {
                const bool changed =
                    next.capability != session_.capability ||
                    next.requires_sudo_for_write != session_.requires_sudo_for_write;
                if (changed) {
                    session_.capability = std::move(next.capability);
                    session_.requires_sudo_for_write = next.requires_sudo_for_write;
                }
                pending_.changed = changed;
                pending_.session = session_;
                pending_.error.clear();
                if (explicit_write_permission(session_)) {
                    active_ = false;
                }
            } else {
                pending_.changed = false;
                pending_.error = error;
            }
        }
    }
}

bool CallGate::update(bool can_write, bool actor_connected) {
    can_write_ = can_write;
    actor_connected_ = actor_connected;
    return consume();
}

bool CallGate::request() {
    requested_ = true;
    return consume();
}

void CallGate::reset() {
    requested_ = false;
}

bool CallGate::consume() {
    if (!requested_ || !can_call()) {
        return false;
    }
    requested_ = false;
    return true;
}

WriteAccessCoordinator::WriteAccessCoordinator(TokenRefresh refresh_token,
                                               ActorConnect connect_actor,
                                               std::chrono::milliseconds retry_interval)
    : refresh_token_(std::move(refresh_token)),
      connect_actor_(std::move(connect_actor)),
      retry_interval_(std::max(std::chrono::milliseconds(1), retry_interval)) {}

void WriteAccessCoordinator::set_write(bool write) {
    if (write_ == write) {
        return;
    }
    write_ = write;
    write_ready_ = false;
    waiting_ = write;
    needs_attempt_ = write;
    attempts_ = 0;
    message_ = write ? "Preparing a write-authorized actor connection" : std::string{};
    next_attempt_ = std::chrono::steady_clock::time_point{};
}

void WriteAccessCoordinator::mark_ready() {
    if (!write_) {
        return;
    }
    write_ready_ = true;
    waiting_ = false;
    needs_attempt_ = false;
    message_.clear();
}

void WriteAccessCoordinator::tick(std::chrono::steady_clock::time_point now) {
    if (!write_ || write_ready_) {
        return;
    }
    if (!needs_attempt_ && now < next_attempt_) {
        return;
    }
    needs_attempt_ = false;
    ++attempts_;

    std::string error;
    if (!refresh_token_ || !refresh_token_(&error)) {
        waiting_ = true;
        message_ = "Refreshing write access; retrying";
        next_attempt_ = now + retry_interval_;
        return;
    }
    if (!connect_actor_ || !connect_actor_(&error)) {
        waiting_ = true;
        message_ = "Reconnecting the actor; retrying";
        next_attempt_ = now + retry_interval_;
        return;
    }
    write_ready_ = true;
    waiting_ = false;
    message_.clear();
}

}  // namespace pk
