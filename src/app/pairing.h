#pragma once

#include "platform/http.h"
#include "platform/secure_store.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
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

    bool restore(CarSession* saved, std::string* error);
    bool start(std::string* error);
    PairingStatus poll(std::string* error);

    bool establish_session(std::string* error);
    bool load_car_session(CarSession* session, std::string* error);
    bool fetch_actor_token(CarSession* session, std::string* error);
    bool persist(const CarSession& session, std::string* error);

    const std::string& pairing_url() const { return pairing_url_; }
    const std::string& remote_hash() const { return remote_hash_; }
    const QrCode& qr() const { return qr_; }
    const std::string& origin() const { return options_.origin; }
    int poll_interval_seconds() const { return options_.poll_interval_seconds; }
    int challenge_generation() const { return challenge_generation_; }

private:
    bool resolve_remote_hash(std::string* error);
    bool regenerate_challenge(std::string* error);
    HttpResponse post_remote(const std::string& function, const std::string& body, std::string* error);
    bool decode_pairing_response(const std::string& body, PairingStatus* status, std::string* error);

    HttpClient& http_;
    SecureStore* store_;
    PairingOptions options_;
    std::string remote_hash_;
    std::string verifier_;
    std::string challenge_;
    std::string pairing_url_;
    QrCode qr_;
    PairingState state_ = PairingState::Idle;
    std::string last_code_;
    int challenge_generation_ = 0;
    std::chrono::steady_clock::time_point challenge_created_;
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
std::string redact_url(const std::string& url);
std::string redact_text(const std::string& text, std::size_t max_length = 400);
std::string redact_json(const std::string& json, std::size_t max_length = 400);

}  // namespace pk
