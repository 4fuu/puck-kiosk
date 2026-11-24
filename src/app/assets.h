#pragma once

#include "platform/http.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pk {

struct AssetFetchStatus {
    bool finished = false;
    bool ok = false;
    int completed = 0;
    int total = 0;
    std::string current;
    std::string error;
};

// Downloads the Puck assets listed in assets/manifest.json into the local
// cache. The manifest is the only committed file; binaries are fetched on
// first run, verified against size and SHA-256, and then used by the Puck
// renderers. A failure leaves whatever the cache already holds and never stops
// the app: the renderer falls back to the placeholder as before.
class AssetFetcher {
public:
    AssetFetcher(HttpClient& http, std::string cache_root = "assets/cache",
                 std::string manifest_path = "assets/manifest.json");
    ~AssetFetcher();

    AssetFetcher(const AssetFetcher&) = delete;
    AssetFetcher& operator=(const AssetFetcher&) = delete;

    void start();
    void wait();
    bool finished() const { return finished_.load(); }
    AssetFetchStatus status() const;

private:
    struct Asset {
        std::string path;
        std::string url;
        std::string sha256;
        long long bytes = 0;
    };

    void run();
    bool load_manifest(std::vector<Asset>* assets, std::string* error) const;
    bool verify_cached(const Asset& asset, std::string* error) const;
    bool download(const Asset& asset, std::string* error);
    void finish(bool ok, const std::string& error);

    HttpClient& http_;
    std::string cache_root_;
    std::string manifest_path_;
    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> ok_{false};
    std::atomic<int> completed_{0};

    mutable std::mutex mutex_;
    int total_ = 0;
    std::string current_;
    std::string error_;
};

}  // namespace pk
