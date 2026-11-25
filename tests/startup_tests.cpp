#include "app/session_startup.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) {
        std::printf("ok   %s\n", label);
        return;
    }
    std::printf("FAIL %s\n", label);
    ++failures;
}

class LeaveHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& request, std::string* error) override {
        (void)error;
        pk::HttpResponse response;
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            deleted = true;
            ++deletes;
            response.status = 204;
            return response;
        }
        response.status = 404;
        return response;
    }

    void clear_cookies() override {
        cleared = true;
        cleared_after_delete = deleted;
        ++clears;
    }

    bool deleted = false;
    bool cleared = false;
    bool cleared_after_delete = false;
    int deletes = 0;
    int clears = 0;
};

class MemoryStore final : public pk::SecureStore {
public:
    bool load(const std::string& key, std::string* value) override {
        const auto found = values.find(key);
        if (found == values.end()) {
            return false;
        }
        if (value) {
            *value = found->second;
        }
        return true;
    }
    bool save(const std::string& key, const std::string& value) override {
        values[key] = value;
        return true;
    }
    bool erase(const std::string& key) override {
        values.erase(key);
        return true;
    }

    std::map<std::string, std::string> values;
};

const char* kCarPage =
    "<html><script>"
    "const page = { type: \"data\", data: { car: { puck: {"
    "thread: { id: \"thread-1\" },"
    "threadActorConfig: { baseURL: \"https://actor.test\", ampURL: \"https://amp.test\","
    " capability: \"write\" }"
    "} } } };"
    "const layout = \"/_app/immutable/nodes/0.js\";"
    "const carNode = \"/_app/immutable/nodes/4.car.js\";"
    "</script></html>";

const char* kSignedOutCarPage =
    "<html><script>"
    "const page = { type: \"data\", data: { car: null } };"
    "const layout = \"/_app/immutable/nodes/0.js\";"
    "const carNode = \"/_app/immutable/nodes/4.car.js\";"
    "</script></html>";

// A layout node and a car node, mirroring the shape of the real /car page: the
// car node is the one that references /api/car/session.
const char* kLayoutNode =
    "import { a } from \"../chunks/layout-only.js\";"
    "export const layout = 1;";

const char* kCarNode =
    "const session = \"/api/car/session\";"
    "import { a } from \"../chunks/c1.js\";"
    "import { b } from \"../chunks/c2.js\";"
    "import { c } from \"../chunks/c3.js\";";

std::string chunk_body(const std::string& name) {
    if (name == "c1" || name == "c2" || name == "c3") {
        return "import { x } from \"./" + name + "-0.js\";"
               "import { y } from \"./" + name + "-1.js\";"
               "import { z } from \"./" + name + "-2.js\";";
    }
    if (name == "c1-0") {
        return "import { x } from \"./c1-0-0.js\";"
               "import { y } from \"./c1-0-1.js\";"
               "import { z } from \"./c1-0-2.js\";";
    }
    if (name == "c1-0-0") {
        return "const remote = \"testhash/claimDevicePairing\";";
    }
    return "export const leaf = 1;";
}

struct RequestRecord {
    std::string url;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point end;
    int connection = 0;
};

class StartupHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& request, std::string* error) override {
        const auto start = std::chrono::steady_clock::now();
        pk::HttpResponse response = respond(request.url, error);
        record(request.url, start, std::chrono::steady_clock::now());
        return response;
    }

    // Models a client that overlaps a batch on a small connection pool and
    // reuses the same connection ids for later batches.
    std::vector<pk::HttpBatchResponse> request_many(
        const std::vector<pk::HttpRequest>& requests) override {
        std::vector<pk::HttpBatchResponse> results(requests.size());
        if (requests.empty()) {
            return results;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batch_sizes.push_back(requests.size());
        }
        if (requests.size() == 1) {
            results[0].response = request(requests[0], &results[0].error);
            return results;
        }

        std::atomic<int> active{0};
        std::vector<std::thread> threads;
        threads.reserve(requests.size());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            threads.emplace_back([&, i] {
                const int now = ++active;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    max_concurrent = std::max(max_concurrent, now);
                }
                const auto start = std::chrono::steady_clock::now();
                results[i].response = respond(requests[i].url, &results[i].error);
                record(requests[i].url, start, std::chrono::steady_clock::now());
                --active;
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        return results;
    }

    pk::HttpResponse respond(const std::string& url, std::string* error) {
        std::this_thread::sleep_for(delay);
        pk::HttpResponse response;
        if (url.find("/_app/immutable/nodes/") != std::string::npos) {
            response.status = 200;
            response.body = url.find("4.car") != std::string::npos ? kCarNode : kLayoutNode;
        } else if (url.find("/_app/immutable/chunks/") != std::string::npos) {
            response.status = 200;
            const std::string name = url.substr(url.rfind('/') + 1, url.size() - url.rfind('/') - 4);
            response.body = chunk_body(name);
        } else if (url.find("/car") != std::string::npos) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++car_requests;
            }
            response.status = 200;
            response.body = signed_out ? kSignedOutCarPage : kCarPage;
        } else {
            response.status = 404;
        }
        (void)error;
        return response;
    }

    void record(const std::string& url, std::chrono::steady_clock::time_point start,
                std::chrono::steady_clock::time_point end) {
        std::lock_guard<std::mutex> lock(mutex_);
        urls.push_back(url);
        records.push_back(RequestRecord{url, start, end, connection});
    }

    std::chrono::milliseconds delay{300};
    bool signed_out = false;
    std::vector<std::string> urls;
    std::vector<RequestRecord> records;
    std::vector<std::size_t> batch_sizes;
    int max_concurrent = 0;
    int car_requests = 0;
    const int connection = 1;

private:
    std::mutex mutex_;
};

struct RecordedCall {
    std::string method;
    std::string url;
};

// A local HTTP stand-in for the pairing/authorization endpoints. A handler can
// block or delay a request so the test can observe the UI-facing calls that
// must stay responsive.
class LocalHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& request, std::string* error) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calls.push_back(RecordedCall{request.method, request.url});
        }
        if (gate) {
            gate(request);
        }
        if (handler) {
            return handler(request, error);
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    }

    bool saw(const std::string& method, const std::string& fragment) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(calls.begin(), calls.end(), [&](const RecordedCall& call) {
            return call.method == method && call.url.find(fragment) != std::string::npos;
        });
    }

    int count(const std::string& method, const std::string& fragment) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int found = 0;
        for (const RecordedCall& call : calls) {
            if (call.method == method && call.url.find(fragment) != std::string::npos) {
                ++found;
            }
        }
        return found;
    }

    std::vector<RecordedCall> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls;
    }

    std::function<pk::HttpResponse(const pk::HttpRequest&, std::string*)> handler;
    std::function<void(const pk::HttpRequest&)> gate;

private:
    mutable std::mutex mutex_;
    std::vector<RecordedCall> calls;
};

pk::PairingOptions options() {
    pk::PairingOptions value;
    value.origin = "https://amp.test";
    value.poll_interval_seconds = 1;
    return value;
}

std::string pairing_reply(const std::string& status, const std::string& code = {}) {
    std::string body = "{\"type\":\"result\",\"data\":{\"status\":\"" + status + "\"";
    if (!code.empty()) {
        body += ",\"code\":\"" + code + "\"";
    }
    body += "}}";
    return body;
}

pk::HttpResponse ok_body(std::string body) {
    pk::HttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    return response;
}

bool wait_for_pairing_state(pk::PairingSession& pairing, pk::PairingState state,
                            pk::PairingUpdate* result,
                            std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        pk::PairingUpdate update;
        while (pairing.take_update(&update)) {
            if (!update.stale && update.status.state == state) {
                if (result) {
                    *result = update;
                }
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool wait_for_authorization(pk::AuthorizationWatcher& watcher, pk::AuthorizationStatus* result,
                            std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (watcher.take(result)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::string car_page_capability(const std::string& capability) {
    std::string page = kCarPage;
    const std::string from = "capability: \"write\"";
    const auto position = page.find(from);
    if (position != std::string::npos) {
        page.replace(position, from.size(), "capability: \"" + capability + "\"");
    }
    return page;
}

bool any_overlap(const std::vector<RequestRecord>& records) {
    for (std::size_t i = 0; i < records.size(); ++i) {
        for (std::size_t j = i + 1; j < records.size(); ++j) {
            if (records[i].start < records[j].end && records[j].start < records[i].end) {
                return true;
            }
        }
    }
    return false;
}

bool all_same_connection(const std::vector<RequestRecord>& records) {
    if (records.empty()) {
        return false;
    }
    const int first = records.front().connection;
    return std::all_of(records.begin(), records.end(),
                       [first](const RequestRecord& record) { return record.connection == first; });
}

void test_restore_does_not_block_start() {
    MemoryStore store;
    store.values["car.session"] =
        R"({"thread_id":"thread-1","actor_base_url":"https://actor.test","capability":"write"})";
    StartupHttp http;
    pk::SessionStartup startup(http, &store, options(), false);

    const auto begin = std::chrono::steady_clock::now();
    startup.start();
    const auto returned =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              begin);
    check(returned < std::chrono::milliseconds(100), "start returns before /car completes");
    check(!startup.status().finished, "session startup is still running after start");
    check(!startup.status().phase.empty(), "the loading state has a phase");

    startup.wait();
    startup.pairing().stop_polling();
    const auto total =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              begin);
    check(total >= std::chrono::milliseconds(250), "wait covers the /car request");
    const pk::SessionStartupStatus status = startup.status();
    check(status.finished && status.resumed && !status.failed, "the saved session restores");
    check(startup.session().thread_id == "thread-1", "the restored thread id is kept");
    check(startup.pairing().qr().empty(), "restoring does not build a QR");
    check(http.car_requests == 1, "validation fetches /car once");
}

void test_pairing_starts_off_the_ui_thread() {
    MemoryStore store;
    StartupHttp http;
    pk::SessionStartup startup(http, &store, options(), false);

    startup.start();
    check(!startup.status().finished, "pairing startup is still running after start");
    startup.wait();
    startup.pairing().stop_polling();

    const pk::SessionStartupStatus status = startup.status();
    check(status.finished && !status.resumed && !status.failed, "an empty store starts pairing");
    check(status.phase == "Waiting for scan", "pairing reports the ready phase");
    check(!startup.pairing().qr().empty(), "the pairing QR is ready after wait");
    check(startup.pairing().remote_hash() == "testhash", "the remote hash was resolved");
    check(http.car_requests == 1, "first-run pairing fetches /car once");
}

void test_force_pair_ignores_the_saved_session() {
    MemoryStore store;
    store.values["car.session"] =
        R"({"thread_id":"thread-1","actor_base_url":"https://actor.test","capability":"write"})";
    store.values["http.cookies"] = "old-cookie";
    StartupHttp http;
    pk::SessionStartup startup(http, &store, options(), true);

    startup.start();
    startup.wait();
    startup.pairing().stop_polling();
    const pk::SessionStartupStatus status = startup.status();
    check(status.finished && !status.resumed && !status.failed, "--pair skips the saved session");
    check(!startup.pairing().qr().empty(), "--pair builds a fresh QR");
    check(store.values.find("car.session") == store.values.end(),
          "a fresh pairing clears the previous saved session");
    check(store.values.find("http.cookies") == store.values.end(),
          "a fresh pairing clears the previous saved cookies");
}

void test_stale_session_reuses_the_car_page() {
    MemoryStore store;
    store.values["car.session"] =
        R"({"thread_id":"thread-1","actor_base_url":"https://actor.test","capability":"write"})";
    StartupHttp http;
    http.signed_out = true;
    pk::SessionStartup startup(http, &store, options(), false);

    startup.start();
    startup.wait();
    startup.pairing().stop_polling();
    const pk::SessionStartupStatus status = startup.status();
    check(status.finished && !status.resumed && !status.failed,
          "a stale saved session falls back to pairing");
    check(!startup.pairing().qr().empty(), "the fallback pairing builds a QR");
    check(http.car_requests == 1, "the stale-session page is not fetched twice");
    check(startup.pairing().remote_hash() == "testhash",
          "the cached page still resolves the remote hash");
}

void test_sibling_chunks_overlap_and_reuse_one_connection() {
    MemoryStore store;
    StartupHttp http;
    http.delay = std::chrono::milliseconds(60);
    pk::SessionStartup startup(http, &store, options(), false);

    const auto begin = std::chrono::steady_clock::now();
    startup.start();
    startup.wait();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              begin);
    startup.pairing().stop_polling();

    check(!startup.pairing().qr().empty(), "the parallel startup builds a QR");
    check(startup.pairing().remote_hash() == "testhash", "the parallel startup resolves the hash");
    check(http.max_concurrent >= 2, "sibling chunk requests overlap in time");
    check(any_overlap(http.records), "recorded requests have overlapping intervals");
    check(all_same_connection(http.records), "every request uses the same connection");
    check(std::any_of(http.batch_sizes.begin(), http.batch_sizes.end(),
                      [](std::size_t size) { return size > 1; }),
          "the startup fetches chunks in batches");
    // Sequential requests would take at least one delay per URL.
    const auto sequential = std::chrono::milliseconds(60) *
                            static_cast<long long>(http.records.size());
    check(elapsed < sequential, "the parallel startup beats a sequential request per URL");
}

void test_slow_pairing_request_does_not_block_the_ui() {
    MemoryStore store;
    LocalHttp http;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool claim_started = false;
    bool release = false;
    http.gate = [&](const pk::HttpRequest& request) {
        if (request.url.find("claimDevicePairing") == std::string::npos) {
            return;
        }
        std::unique_lock<std::mutex> lock(gate_mutex);
        claim_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release; });
    };
    http.handler = [](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            return ok_body(pairing_reply("pending"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the local pairing session starts");
    pairing.start_polling();
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        check(gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return claim_started; }),
              "the background poll reaches the slow claim");
    }

    const auto qr_begin = std::chrono::steady_clock::now();
    const pk::QrCode qr = pairing.qr();
    const int generation = pairing.challenge_generation();
    const auto qr_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - qr_begin);
    check(!qr.empty(), "the QR snapshot is available while the claim is blocked");
    check(qr_elapsed < std::chrono::milliseconds(100),
          "reading the QR does not wait for the slow pairing request");

    pk::PairingUpdate update;
    const auto take_begin = std::chrono::steady_clock::now();
    const bool took_update = pairing.take_update(&update);
    const auto take_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - take_begin);
    check(!took_update, "a blocked claim has no result to consume yet");
    check(take_elapsed < std::chrono::milliseconds(50),
          "consuming pairing updates never waits for HTTP");

    const auto restart_begin = std::chrono::steady_clock::now();
    check(pairing.restart(&error), "Try Again restarts the local challenge");
    const auto restart_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - restart_begin);
    check(restart_elapsed < std::chrono::milliseconds(100),
          "Try Again does not wait for the in-flight claim");
    check(pairing.challenge_generation() > generation, "Try Again advances the round");

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();
    pairing.stop_polling();
}

void test_scanned_code_survives_the_unscanned_expiry() {
    MemoryStore store;
    LocalHttp http;
    http.handler = [](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            return ok_body(pairing_reply("scanned", "123456"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingOptions pairing_options = options();
    pairing_options.challenge_lifetime_seconds = 1;
    pk::PairingSession pairing(http, &store, pairing_options);
    std::string error;
    check(pairing.start(&error), "the expiring local session starts");
    pairing.start_polling();

    pk::PairingUpdate first;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &first),
          "the scanned code arrives");
    const int generation = first.generation;
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));

    pk::PairingUpdate later;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &later),
          "the scanned challenge keeps polling past the unscanned expiry");
    pairing.stop_polling();
    check(later.generation == generation,
          "a scanned challenge is not replaced at the unscanned expiry point");
    check(later.status.code == "123456", "the confirmation code survives the expiry point");
    check(!later.qr.empty(), "the scanned round still has its QR");
}

void test_pending_returns_the_display_to_the_qr() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            if (++claims == 1) {
                return ok_body(pairing_reply("scanned", "654321"));
            }
            return ok_body(pairing_reply("pending"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the pending local session starts");
    pairing.start_polling();

    pk::PairingUpdate scanned;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &scanned),
          "the first code arrives");
    pk::PairingUpdate pending;
    check(wait_for_pairing_state(pairing, pk::PairingState::Waiting, &pending),
          "an explicit pending reply returns the pairing state to waiting");
    pairing.stop_polling();
    check(pending.status.code.empty(), "pending clears the old confirmation code");
    check(pending.generation == scanned.generation,
          "pending keeps the same challenge instead of silently rotating it");
    check(!pending.qr.empty(), "pending has a QR to show again");
}

void test_restart_after_unconsumed_paired_keeps_polling_and_discards() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            if (++claims == 1) {
                return ok_body(pairing_reply("paired"));
            }
            return ok_body(pairing_reply("scanned", "second-code"));
        }
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            pk::HttpResponse response;
            response.status = 204;
            return response;
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the unconsumed-paired local session starts");
    pairing.start_polling();

    bool paired_pending = false;
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < wait_deadline) {
        if (pairing.paired_pending()) {
            paired_pending = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(paired_pending, "the worker publishes Paired before the UI consumes it");
    check(pairing.can_retry(), "the visible code screen can still retry before consuming Paired");

    const int old_generation = pairing.challenge_generation();
    check(pairing.restart(&error), "Try Again works on the unconsumed Paired snapshot");
    const int new_generation = pairing.challenge_generation();

    bool saw_new_code = false;
    bool saw_paired = false;
    pk::PairingUpdate new_update;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        pk::PairingUpdate update;
        while (pairing.take_update(&update)) {
            if (update.status.state == pk::PairingState::Paired) {
                saw_paired = true;
            }
            if (update.status.state == pk::PairingState::Scanned &&
                update.status.code == "second-code") {
                saw_new_code = true;
                new_update = update;
            }
        }
        if (saw_new_code) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    pairing.stop_polling();
    check(saw_new_code, "the replacement round is polled after an unconsumed Paired");
    check(!saw_paired, "the unconsumed Paired snapshot does not leak into the new round");
    check(new_generation == old_generation + 1, "the replacement has a new generation");
    check(new_update.generation == new_generation, "the new code belongs to the new round");
    check(http.saw("DELETE", "/api/car/session"),
          "the unconsumed Paired session is discarded before the new round");
    check(http.count("POST", "claimDevicePairing") >= 2,
          "the worker polls the replacement challenge");
}

void test_expired_inflight_paired_reply_is_published_before_cleanup() {
    MemoryStore store;
    LocalHttp http;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool claim_started = false;
    bool release = false;
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            claim_started = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release; });
            return ok_body(pairing_reply("paired"));
        }
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            pk::HttpResponse response;
            response.status = 204;
            return response;
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the slow-reply local session starts");
    pairing.start_polling();
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        check(gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return claim_started; }),
              "the slow reply is in flight");
    }

    const auto expire_begin = std::chrono::steady_clock::now();
    check(pairing.expire("Pairing timed out"), "the timeout expires the pairing");
    pk::PairingUpdate expired;
    bool saw_expired = false;
    const auto take_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < take_deadline) {
        if (pairing.take_update(&expired) &&
            expired.status.state == pk::PairingState::Expired) {
            saw_expired = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto expire_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - expire_begin);
    check(saw_expired, "the expired snapshot is available while the old claim is blocked");
    check(expire_elapsed < std::chrono::milliseconds(100),
          "expire does not wait for the in-flight claim");
    check(!http.saw("DELETE", "/api/car/session"),
          "the old session is not deleted before its paired reply arrives");

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();
    const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < cleanup_deadline &&
           !http.saw("DELETE", "/api/car/session")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pairing.stop_polling();
    check(http.saw("DELETE", "/api/car/session"),
          "the late paired reply cleans its session after the terminal snapshot");
    pk::PairingUpdate late;
    bool saw_late_paired = false;
    while (pairing.take_update(&late)) {
        if (late.status.state == pk::PairingState::Paired) {
            saw_late_paired = true;
        }
    }
    check(!saw_late_paired, "the late paired reply does not replace the terminal state");
}

void test_expired_slow_claim_cleans_before_the_new_round_claim() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool claim_started = false;
    bool release = false;
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            const int claim = ++claims;
            if (claim == 1) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                claim_started = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&] { return release; });
                return ok_body(pairing_reply("paired"));
            }
            return ok_body(pairing_reply("scanned", "after-expire"));
        }
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            pk::HttpResponse response;
            response.status = 204;
            return response;
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the expiry/retry local session starts");
    pairing.start_polling();
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        check(gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return claim_started; }),
              "the first claim is in flight");
    }
    check(pairing.expire("Pairing timed out"), "the first round times out");
    const int expired_generation = pairing.challenge_generation();
    check(pairing.restart(&error), "Try Again replaces the expired round before the old reply");
    const int new_generation = pairing.challenge_generation();
    check(new_generation == expired_generation + 1, "the retry has a new generation");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();

    pk::PairingUpdate restarted;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &restarted),
          "the new round receives its own scanned reply");
    pairing.stop_polling();
    check(restarted.status.code == "after-expire", "the post-expiry code belongs to the new round");

    const std::vector<RecordedCall> calls = http.snapshot();
    int delete_index = -1;
    int second_claim_index = -1;
    int claim_index = 0;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].method == "DELETE" &&
            calls[i].url.find("/api/car/session") != std::string::npos) {
            delete_index = static_cast<int>(i);
        }
        if (calls[i].method == "POST" &&
            calls[i].url.find("claimDevicePairing") != std::string::npos &&
            ++claim_index == 2) {
            second_claim_index = static_cast<int>(i);
        }
    }
    check(delete_index >= 0, "the old paired session is deleted");
    check(second_claim_index >= 0, "the new round sends a claim");
    check(delete_index < second_claim_index,
          "the old DELETE completes before the new round claim starts");
}

void test_expire_then_stop_cleans_an_unconsumed_paired_session() {
    MemoryStore store;
    LocalHttp http;
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            return ok_body(pairing_reply("paired"));
        }
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            pk::HttpResponse response;
            response.status = 204;
            return response;
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the expire/stop local session starts");
    pairing.start_polling();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !pairing.paired_pending()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(pairing.paired_pending(), "the success is published before the app stops");
    check(pairing.expire("Pairing timed out"), "the terminal state replaces the pending success");
    pairing.stop_polling();
    check(http.saw("DELETE", "/api/car/session"),
          "stopping after a terminal state cleans the unconsumed paired session");
}

void test_paired_acknowledgement_keeps_the_session() {
    MemoryStore store;
    LocalHttp http;
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            return ok_body(pairing_reply("paired"));
        }
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            pk::HttpResponse response;
            response.status = 204;
            return response;
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the acknowledged-paired local session starts");
    pairing.start_polling();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !pairing.paired_pending()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(pairing.paired_pending(), "the successful snapshot is waiting for the app");
    check(pairing.acknowledge_paired(), "the app can take ownership of the paired session");
    check(!pairing.paired_pending(), "an acknowledged pair is no longer a retry candidate");
    pairing.stop_polling();
    check(!http.saw("DELETE", "/api/car/session"),
          "an acknowledged paired session is not deleted");
}

void test_terminal_pairing_rejection_waits_for_restart() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            if (++claims == 1) {
                return ok_body(
                    R"({"type":"error","error":{"message":"pairing rejected"}})");
            }
            return ok_body(pairing_reply("scanned", "after-reject"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the terminal-rejection local session starts");
    pairing.start_polling();

    pk::PairingUpdate failed;
    check(wait_for_pairing_state(pairing, pk::PairingState::Failed, &failed),
          "a terminal rejection publishes Failed");
    check(failed.can_retry, "the terminal rejection is retryable");
    check(pairing.can_retry(), "the pairer reports the terminal rejection as retryable");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int stopped_claims = claims.load();
    check(stopped_claims == 1, "a terminal rejection stops automatic polling");

    check(pairing.restart(&error), "Try Again revives a terminated worker");
    pk::PairingUpdate restarted;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &restarted),
          "the replacement challenge is polled after terminal rejection");
    pairing.stop_polling();
    check(restarted.status.code == "after-reject", "the replacement code arrives");
    check(claims.load() >= 2, "the replacement round sends a new claim");
}

void test_http_pairing_rejection_is_terminal_and_retryable() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            if (++claims == 1) {
                pk::HttpResponse response;
                response.status = 403;
                response.body = "forbidden";
                return response;
            }
            return ok_body(pairing_reply("scanned", "after-403"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the HTTP-rejection local session starts");
    pairing.start_polling();
    pk::PairingUpdate failed;
    check(wait_for_pairing_state(pairing, pk::PairingState::Failed, &failed),
          "an HTTP 403 rejection publishes Failed");
    check(pairing.restart(&error), "restart revives after an HTTP 403 rejection");
    pk::PairingUpdate restarted;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &restarted),
          "the post-403 challenge is polled");
    check(restarted.status.code == "after-403", "the post-403 code arrives");
    pairing.stop_polling();
}

void test_pairing_expiry_is_retryable() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<bool> scanned{false};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            return ok_body(scanned ? pairing_reply("scanned", "after-timeout")
                                   : pairing_reply("pending"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the expiring local session starts");
    pairing.start_polling();
    pk::PairingUpdate waiting;
    check(wait_for_pairing_state(pairing, pk::PairingState::Waiting, &waiting),
          "the pairer starts polling");

    check(pairing.expire("Pairing timed out"), "the overall timeout expires the pairing");
    check(pairing.can_retry(), "the expired pairing is retryable");
    const int expired_generation = pairing.challenge_generation();
    pairing.stop_polling();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    scanned = true;
    check(pairing.restart(&error), "restart revives the expired pairer");
    check(pairing.challenge_generation() > expired_generation,
          "restart creates a new challenge after timeout");
    pk::PairingUpdate restarted;
    check(wait_for_pairing_state(pairing, pk::PairingState::Scanned, &restarted),
          "the expired pairing can scan a new challenge");
    check(restarted.status.code == "after-timeout", "the post-timeout code arrives");
    pairing.stop_polling();
}

void test_write_access_waits_for_a_new_authorized_socket() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> token_calls{0};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("/api/thread-actors") != std::string::npos) {
            if (++token_calls == 1) {
                pk::HttpResponse response;
                response.status = 500;
                response.body = "temporary token failure";
                return response;
            }
            return ok_body(R"({"wsToken":"write-token"})");
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    pk::CarSession session;
    session.thread_id = "thread-1";
    session.actor_base_url = "https://actor.test";
    session.actor_amp_url = "https://amp.test";
    session.capability = "write";

    std::atomic<int> connect_attempts{0};
    std::atomic<int> successful_connections{0};
    int active_socket = 1;
    int old_socket_starts = 0;
    int new_socket_starts = 0;
    pk::WriteAccessCoordinator access(
        [&](std::string* error) { return pairing.fetch_actor_token(&session, error); },
        [&](std::string* error) {
            const int attempt = ++connect_attempts;
            if (attempt == 1) {
                if (error) {
                    *error = "actor refused the connection";
                }
                return false;
            }
            ++successful_connections;
            active_socket = 2;
            return true;
        },
        std::chrono::milliseconds(500));

    check(!access.write_ready(), "write access is closed before the new socket exists");
    access.set_write(true);
    const auto start = std::chrono::steady_clock::now();
    access.tick(start);
    check(access.waiting(), "the token failure leaves the gate waiting");
    check(!access.write_ready(), "a token failure does not mark the old socket ready");
    check(token_calls.load() == 1, "the first token request fails");
    check(connect_attempts.load() == 0, "no socket is created without a write token");

    pk::CallGate gate;
    check(!gate.request(), "a call request waits while the write socket is not ready");
    check(!gate.update(access.write_ready(), true),
          "the old read socket cannot release the queued call");

    access.tick(start + std::chrono::milliseconds(400));
    check(token_calls.load() == 1, "the token retry is throttled");
    access.tick(start + std::chrono::milliseconds(600));
    check(token_calls.load() == 2, "the token request is retried");
    check(connect_attempts.load() == 1, "a reconnect is attempted after the token arrives");
    check(!access.write_ready(), "a failed actor connection keeps the gate closed");
    check(!gate.update(access.write_ready(), true),
          "a failed actor connection does not release the call");

    access.tick(start + std::chrono::milliseconds(1200));
    check(token_calls.load() == 3, "the token is refreshed again for the next attempt");
    check(connect_attempts.load() == 2, "the actor connection is retried");
    check(successful_connections.load() == 1, "the new write socket is established once");
    check(access.write_ready(), "the gate opens only after the new write socket is ready");

    check(gate.update(access.write_ready(), true),
          "the queued --call request is released after the write socket is ready");
    if (active_socket == 1) {
        ++old_socket_starts;
    } else {
        ++new_socket_starts;
    }
    check(old_socket_starts == 0, "the old read socket never receives the call start");
    check(new_socket_starts == 1, "the new write socket receives exactly one call start");
    check(!gate.update(access.write_ready(), true),
          "the released request is not started a second time");
    access.tick(start + std::chrono::milliseconds(2000));
    check(connect_attempts.load() == 2, "a ready write access does not reconnect again");

    pk::WriteAccessCoordinator preopened(
        [](std::string*) { return true; }, [](std::string*) { return true; });
    preopened.set_write(true);
    preopened.mark_ready();
    check(preopened.write_ready() && !preopened.waiting(),
          "an initially write-authorized socket is ready without another connect");
}

void test_transient_pairing_error_keeps_polling() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            if (++claims == 1) {
                pk::HttpResponse response;
                response.status = 500;
                response.body = "temporary";
                return response;
            }
            return ok_body(pairing_reply("scanned", "999999"));
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the transient-error local session starts");
    pairing.start_polling();

    bool saw_error = false;
    bool saw_code = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        pk::PairingUpdate update;
        while (pairing.take_update(&update)) {
            if (update.status.message.find("HTTP 500") != std::string::npos) {
                saw_error = true;
            }
            if (update.status.state == pk::PairingState::Scanned &&
                update.status.code == "999999") {
                saw_code = true;
            }
        }
        if (saw_code) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    pairing.stop_polling();
    check(saw_error, "a transient pairing HTTP error is surfaced without exiting");
    check(saw_code, "the poller retries after the transient pairing error");
    check(claims >= 2, "the failed claim is retried");
}

void test_retry_isolates_a_late_paired_reply_and_discards_its_session() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> claims{0};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool claim_started = false;
    bool release = false;
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("claimDevicePairing") != std::string::npos) {
            const int claim = ++claims;
            if (claim == 1) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                claim_started = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&] { return release; });
                return ok_body(pairing_reply("paired"));
            }
            return ok_body(pairing_reply("scanned", "new-code"));
        }
        if (request.method == "DELETE" &&
            request.url.find("/api/car/session") != std::string::npos) {
            pk::HttpResponse response;
            response.status = 204;
            return response;
        }
        pk::HttpResponse response;
        response.status = 404;
        return response;
    };

    pk::PairingSession pairing(http, &store, options());
    std::string error;
    check(pairing.start(&error), "the retry local session starts");
    pairing.start_polling();
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        check(gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return claim_started; }),
              "the first round's claim is in flight");
    }

    const int old_generation = pairing.challenge_generation();
    check(pairing.restart(&error), "Try Again replaces the in-flight round");
    const int new_generation = pairing.challenge_generation();
    check(new_generation == old_generation + 1, "the replacement round has a new generation");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();

    bool saw_new_code = false;
    bool saw_paired = false;
    pk::PairingUpdate new_update;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        pk::PairingUpdate update;
        while (pairing.take_update(&update)) {
            if (update.stale) {
                continue;
            }
            if (update.status.state == pk::PairingState::Paired) {
                saw_paired = true;
            }
            if (update.status.state == pk::PairingState::Scanned &&
                update.status.code == "new-code") {
                saw_new_code = true;
                new_update = update;
            }
        }
        if (saw_new_code) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    pairing.stop_polling();
    check(saw_new_code, "the new round receives its own scanned reply");
    check(!saw_paired, "a late paired reply from the abandoned round is not published");
    check(new_update.generation == new_generation, "the new code belongs to the new round");
    check(http.saw("DELETE", "/api/car/session"),
          "the late paired reply deletes its stale car session");
    check(http.count("POST", "claimDevicePairing") >= 2,
          "the new round polls after Try Again");
}

void test_authorization_refresh_releases_one_queued_call() {
    MemoryStore store;
    LocalHttp http;
    std::atomic<int> car_requests{0};
    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool first_request_started = false;
    http.handler = [&](const pk::HttpRequest& request, std::string*) {
        if (request.url.find("/car") == std::string::npos) {
            pk::HttpResponse response;
            response.status = 404;
            return response;
        }
        const int call = ++car_requests;
        {
            std::lock_guard<std::mutex> lock(start_mutex);
            first_request_started = true;
        }
        start_cv.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return ok_body(car_page_capability(call == 1 ? "read" : "write"));
    };

    pk::PairingSession pairing(http, &store, options());
    pk::CarSession session;
    session.thread_id = "thread-1";
    session.actor_base_url = "https://actor.test";
    session.capability = "read";

    pk::AuthorizationWatcher watcher(pairing, std::chrono::milliseconds(40));
    const auto watch_begin = std::chrono::steady_clock::now();
    watcher.start(session);
    const auto watch_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - watch_begin);
    check(watch_elapsed < std::chrono::milliseconds(100),
          "the authorization watch starts off the UI thread");
    {
        std::unique_lock<std::mutex> lock(start_mutex);
        check(start_cv.wait_for(lock, std::chrono::seconds(2),
                                [&] { return first_request_started; }),
              "the authorization watch requests /car");
    }

    pk::AuthorizationStatus early;
    const auto take_begin = std::chrono::steady_clock::now();
    const bool early_result = watcher.take(&early);
    const auto take_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - take_begin);
    check(!early_result, "a slow /car refresh has no result yet");
    check(take_elapsed < std::chrono::milliseconds(50),
          "reading authorization updates never waits for /car");

    pk::AuthorizationStatus read_status;
    check(wait_for_authorization(watcher, &read_status),
          "the read-only authorization snapshot arrives");
    check(read_status.session.capability == "read", "the initial capability is read-only");
    check(!read_status.changed, "the initial snapshot is not reported as a change");

    pk::AuthorizationStatus write_status;
    check(wait_for_authorization(watcher, &write_status),
          "the post-passkey authorization snapshot arrives");
    check(write_status.changed, "the read-to-write authorization change is reported");
    check(write_status.session.capability == "write", "the refreshed capability is write");
    check(pk::explicit_write_permission(write_status.session),
          "write access is explicit rather than the empty-capability default");

    pk::CallGate gate;
    check(!gate.request(), "a call request waits while access is read-only");
    check(gate.pending(), "the explicit call request is remembered");
    check(!gate.update(true, false),
          "an actor that is still disconnected does not release the request");
    check(gate.update(true, true),
          "the gate releases the queued explicit request exactly once");
    check(!gate.update(true, true), "later authorization updates do not start another call");
    check(!gate.pending(), "the released request is consumed");

    pk::CallGate open_gate;
    check(!open_gate.update(true, true), "update alone does not start an unrequested call");
    check(open_gate.request(), "a fresh UI click starts immediately when the gate is open");
    check(!open_gate.update(true, true), "the immediate request also starts only once");

    watcher.stop();
    check(http.count("GET", "/car") == 2,
          "authorization polling stops after write access is observed");
}

void test_leave_session_deletes_then_clears_local_state() {
    MemoryStore store;
    store.values["car.session"] = R"({"thread_id":"old-user"})";
    store.values["http.cookies"] = "car=old-user";
    LeaveHttp http;
    pk::PairingSession pairing(http, &store, options());

    std::string error;
    pairing.leave_session(&error);
    check(error.empty(), "session leave does not report an error on HTTP 204");
    check(http.deletes == 1, "session leave sends one DELETE");
    check(http.cleared, "session leave clears the in-memory cookie jar");
    check(http.cleared_after_delete,
          "the DELETE uses the session cookie before the jar is erased");
    check(store.values.count("car.session") == 0,
          "the saved car session is erased");
    check(store.values.count("http.cookies") == 0,
          "the saved cookie jar is erased");
}

}  // namespace

int main() {
    test_restore_does_not_block_start();
    test_pairing_starts_off_the_ui_thread();
    test_force_pair_ignores_the_saved_session();
    test_stale_session_reuses_the_car_page();
    test_sibling_chunks_overlap_and_reuse_one_connection();
    test_slow_pairing_request_does_not_block_the_ui();
    test_scanned_code_survives_the_unscanned_expiry();
    test_pending_returns_the_display_to_the_qr();
    test_restart_after_unconsumed_paired_keeps_polling_and_discards();
    test_expired_inflight_paired_reply_is_published_before_cleanup();
    test_expired_slow_claim_cleans_before_the_new_round_claim();
    test_expire_then_stop_cleans_an_unconsumed_paired_session();
    test_paired_acknowledgement_keeps_the_session();
    test_terminal_pairing_rejection_waits_for_restart();
    test_http_pairing_rejection_is_terminal_and_retryable();
    test_pairing_expiry_is_retryable();
    test_write_access_waits_for_a_new_authorized_socket();
    test_transient_pairing_error_keeps_polling();
    test_retry_isolates_a_late_paired_reply_and_discards_its_session();
    test_authorization_refresh_releases_one_queued_call();
    test_leave_session_deletes_then_clears_local_state();
    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
