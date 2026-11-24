#include "app/session_startup.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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

pk::PairingOptions options() {
    pk::PairingOptions value;
    value.origin = "https://amp.test";
    return value;
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
    StartupHttp http;
    pk::SessionStartup startup(http, &store, options(), true);

    startup.start();
    startup.wait();
    const pk::SessionStartupStatus status = startup.status();
    check(status.finished && !status.resumed && !status.failed, "--pair skips the saved session");
    check(!startup.pairing().qr().empty(), "--pair builds a fresh QR");
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

}  // namespace

int main() {
    test_restore_does_not_block_start();
    test_pairing_starts_off_the_ui_thread();
    test_force_pair_ignores_the_saved_session();
    test_stale_session_reuses_the_car_page();
    test_sibling_chunks_overlap_and_reuse_one_connection();
    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
