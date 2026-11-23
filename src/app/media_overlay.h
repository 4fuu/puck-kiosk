#pragma once

#include "core/types.h"
#include "render/canvas.h"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pk {

class HttpClient;

struct MediaItem {
    std::string id;
    std::string url;  // rewritten to the app origin for actor items
    std::string mime_type;
    std::string caption;
    std::string expires_at;
    std::int64_t expires_at_ms = 0;  // 0 means no expiry
    std::int64_t size_bytes = 0;
};

struct MediaPresentation {
    std::string id;
    std::string title;
    std::vector<MediaItem> items;
};

// The overlay's hit rects, shared by draw() and the car UI's hit map.
struct MediaButtons {
    Rect close;
    Rect previous;
    Rect next;
    bool navigation = false;
    bool can_previous = false;
    bool can_next = false;
};

// The full-stage media overlay from the car page. The actor sends a nullable
// presentation; this class owns its lifecycle, fetches each picture with the
// session cookie, decodes off the UI thread, and keeps at most a few decoded
// textures. Puck keeps drawing underneath; the overlay only covers the stage.
class MediaOverlay {
public:
    MediaOverlay(HttpClient* http, std::string app_origin, Canvas& canvas);
    ~MediaOverlay();

    MediaOverlay(const MediaOverlay&) = delete;
    MediaOverlay& operator=(const MediaOverlay&) = delete;

    void on_actor_message(const std::string& text);

    // Accepts the actor's `presentation` value: null closes the overlay.
    // Local paths are only allowed for the headless demo hook.
    bool set_presentation(const nlohmann::json& presentation, bool allow_local_paths,
                          std::string* error);

    void close();
    void next();
    void previous();
    void set_index(int index);

    // Uploads finished decodes and releases evicted textures. Call once per
    // frame on the UI thread, before draw().
    void poll();
    void draw(Canvas& canvas, Rect stage, bool compact = false) const;
    MediaButtons buttons(Canvas& canvas, Rect stage, bool compact = false) const;

    bool visible() const;
    bool settled() const;
    // True while a fetch is queued, running, or waiting to be uploaded. The
    // actor loop defers session renewals while the shared HTTP client is busy.
    bool busy() const;
    int index() const { return index_; }
    const MediaPresentation* presentation() const { return presentation_ ? &*presentation_ : nullptr; }
    std::string current_error() const;
    std::string status_text() const;

private:
    struct FetchRequest {
        std::string key;
        std::string url;
        std::string local_path;
        std::int64_t max_bytes = 0;
        std::uint64_t generation = 0;
    };

    struct FetchResult {
        std::string key;
        std::uint64_t generation = 0;
        bool ok = false;
        std::string error;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
    };

    struct CachedImage {
        std::string key;
        ImageHandle handle = kInvalidImage;
        int width = 0;
        int height = 0;
        std::uint64_t last_used = 0;
    };

    struct ItemStatus {
        bool loading = false;
        std::string error;
    };

    void worker_main();
    FetchResult fetch(const FetchRequest& request);
    void enqueue(const MediaItem& item);
    void request_current_and_next();
    void check_expiry();
    void clear_images();
    void clear_work();
    void evict_if_needed();
    void store_result(FetchResult result);
    ImageHandle find_image(const std::string& key) const;
    const MediaItem* current_item() const;
    const MediaItem* find_item(const std::string& id) const;
    void log_state(const char* prefix) const;

    HttpClient* http_ = nullptr;
    std::string app_origin_;
    Canvas& canvas_;

    std::optional<MediaPresentation> presentation_;
    int index_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t use_counter_ = 0;
    std::vector<CachedImage> images_;
    std::vector<ItemStatus> statuses_;

    mutable std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::deque<FetchRequest> requests_;
    std::deque<FetchResult> results_;
    bool worker_busy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace pk
