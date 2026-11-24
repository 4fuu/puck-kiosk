#include "app/session_startup.h"

#include <chrono>
#include <cstdio>
#include <map>
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
    "const node = \"/_app/immutable/nodes/0.js\";"
    "</script></html>";

const char* kCarNode = "const remote = \"testhash/claimDevicePairing\";";

class StartupHttp final : public pk::HttpClient {
public:
    pk::HttpResponse request(const pk::HttpRequest& request, std::string* error) override {
        urls.push_back(request.url);
        std::this_thread::sleep_for(delay);
        pk::HttpResponse response;
        if (request.url.find("/_app/immutable/nodes/") != std::string::npos) {
            response.status = 200;
            response.body = kCarNode;
        } else if (request.url.find("/car") != std::string::npos) {
            response.status = 200;
            response.body = kCarPage;
        } else {
            response.status = 404;
        }
        (void)error;
        return response;
    }

    std::chrono::milliseconds delay{300};
    std::vector<std::string> urls;
};

pk::PairingOptions options() {
    pk::PairingOptions value;
    value.origin = "https://amp.test";
    return value;
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

}  // namespace

int main() {
    test_restore_does_not_block_start();
    test_pairing_starts_off_the_ui_thread();
    test_force_pair_ignores_the_saved_session();
    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
