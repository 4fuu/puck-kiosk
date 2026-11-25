#pragma once

#include "platform/http.h"
#include "platform/secure_store.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pk {

struct QrCode {
    int size = 0;
    std::vector<std::uint8_t> modules;

    bool empty() const { return size <= 0 || modules.size() != static_cast<std::size_t>(size) * size; }
    bool dark(int x, int y) const {
        return !empty() && modules[static_cast<std::size_t>(y) * size + x] != 0;
    }
};

enum class PairingState {
    Idle,
    Waiting,
    Scanned,
    Paired,
    Expired,
    Failed,
};

struct PairingStatus {
    PairingState state = PairingState::Idle;
    std::string code;
    std::string message;
    // The server rejected the challenge rather than failing transiently.
    bool terminal = false;
};

// One snapshot from the background pairing poller. `challenge_changed` is set
// when the QR changed since the previous snapshot; `stale` marks a reply from a
// round the user already replaced, which the app must ignore.
struct PairingUpdate {
    bool valid = false;
    bool challenge_changed = false;
    bool stale = false;
    bool can_retry = false;
    int generation = 0;
    PairingStatus status;
    QrCode qr;
    std::string pairing_url;
};

struct CarSession {
    std::string thread_id;
    std::string actor_base_url;
    std::string actor_amp_url;
    std::string actor_pool;
    std::string capability;
    bool requires_sudo_for_write = false;
    std::string ws_token;
};

struct PairingOptions {
    std::string origin = "https://ampcode.com";
    std::string fallback_hash = "1rbu0j0";
    int poll_interval_seconds = 2;
    int challenge_lifetime_seconds = 240;
};

// Owns the pairing challenge and the HTTP calls around it. The app drives the
// state machine and renders the QR; this class only talks to ampcode.com.
class PairingSession {
public:
    PairingSession(HttpClient& http, SecureStore* store, PairingOptions options = {});
    ~PairingSession();

    PairingSession(const PairingSession&) = delete;
    PairingSession& operator=(const PairingSession&) = delete;

    bool restore(CarSession* saved, std::string* error);
    bool start(std::string* error);

    // Claims the challenge on a worker thread. The UI loop never performs the
    // pairing HTTP request and only consumes snapshots with take_update().
    void start_polling();
    void stop_polling();
    bool take_update(PairingUpdate* update);

    // Try Again: abandons the current challenge and publishes a fresh QR. A
    // reply from the abandoned round cannot change the new round's state and
    // a late "paired" reply deletes that stale car session.
    bool restart(std::string* error);
    bool can_retry() const;
    bool paired_pending() const;
    // The app has taken ownership of the paired session and is about to
    // establish/load it. A restart before this point must discard the session.
    bool acknowledge_paired();
    // Overall timeout or a terminal server rejection; polling stops until
    // restart() replaces the challenge.
    bool expire(const std::string& message);

    bool establish_session(std::string* error);
    bool load_car_session(CarSession* session, std::string* error);
    // Re-reads /car and updates the explicit capability without touching the
    // actor URL or websocket token. Used by the read-only authorization watch.
    bool refresh_authorization(CarSession* session, std::string* error);
    bool fetch_actor_token(CarSession* session, std::string* error);
    bool persist(const CarSession& session, std::string* error);
    // Shared-device exit: DELETE the temporary car session while its cookie is
    // still valid, then erase the saved session and the in-memory cookie jar.
    void leave_session(std::string* error);

    std::string pairing_url() const;
    std::string remote_hash() const;
    QrCode qr() const;
    const std::string& origin() const { return options_.origin; }
    int poll_interval_seconds() const { return options_.poll_interval_seconds; }
    int challenge_generation() const;

private:
    bool resolve_remote_hash(std::string* error);
    bool regenerate_challenge(std::string* error);
    bool regenerate_challenge_locked(std::string* error);
    void set_remote_hash(std::string hash);
    bool poll_once();
    void run_poll_loop();
    bool take_discard_request();
    void publish_update_locked(const PairingStatus& status, bool stale = false);
    void discard_stale_session();
    void discard_stale_session_locked();
    HttpResponse post_remote(const std::string& function, const std::string& body, std::string* error);
    bool decode_pairing_response(const std::string& body, PairingStatus* status, std::string* error);

    HttpClient& http_;
    SecureStore* store_;
    PairingOptions options_;
    mutable std::mutex state_mutex_;
    std::string remote_hash_;
    std::string verifier_;
    std::string challenge_;
    std::string pairing_url_;
    // The /car page fetched while validating a saved session. Reused for
    // remote-hash discovery when that session turns out to be unusable, so a
    // first-run pairing does not fetch the same page twice.
    std::string cached_car_page_;
    QrCode qr_;
    PairingState state_ = PairingState::Idle;
    std::string last_code_;
    std::string last_poll_error_;
    int challenge_generation_ = 0;
    int paired_generation_ = 0;
    bool paired_unacknowledged_ = false;
    bool discard_pending_ = false;
    std::chrono::steady_clock::time_point challenge_created_;

    std::mutex update_mutex_;
    PairingUpdate latest_;
    int published_generation_ = -1;

    // Serializes an old session DELETE with the next round's claim POST so a
    // retry can never attach the new QR to the user who was just discarded.
    std::mutex round_mutex_;

    std::mutex poll_lifecycle_mutex_;
    std::mutex poll_wait_mutex_;
    std::condition_variable poll_wake_;
    std::thread poll_thread_;
    bool poll_running_ = false;
    bool poll_stop_ = false;
    bool poll_wakeup_ = false;
};

std::string base64_encode(const std::uint8_t* data, std::size_t size);
std::string base64url_encode(const std::uint8_t* data, std::size_t size);
std::string random_base64url(std::size_t bytes);
std::string random_uuid();
QrCode make_qr_code(const std::string& text);

struct ActorEndpoint {
    std::string url;
    std::vector<std::string> subprotocols;
    std::string display_url;
};

bool build_actor_endpoint(const CarSession& session, const std::string& page_load_id,
                          ActorEndpoint* endpoint, std::string* error);
bool wait_actor_ready(HttpClient& http, const CarSession& session, const std::string& page_load_id,
                      std::string* error);
double ws_token_exp_seconds(const std::string& token);
std::string redact_url(const std::string& url);
std::string redact_text(const std::string& text, std::size_t max_length = 400);
std::string redact_json(const std::string& json, std::size_t max_length = 400);

}  // namespace pk
