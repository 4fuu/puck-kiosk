#include "voice/loopback.h"

#include "core/log.h"
#include "voice/engine.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace pk::voice {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t read_rss_kb(const char* key) {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind(key, 0) != 0) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        try {
            return static_cast<std::uint64_t>(std::stoull(line.substr(colon + 1)));
        } catch (...) {
            return 0;
        }
    }
#else
    (void)key;
#endif
    return 0;
}

long long elapsed_ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

void print_stats(const char* label, const VoiceEngineStats& stats) {
    std::printf(
        "[loopback] %s sent=%llu received=%llu decoded=%llu concealed=%llu late=%llu reordered=%llu "
        "duplicates=%llu nack=%llu retransmit=%llu dropped=%llu jitter=%d frames buffered=%d "
        "in=%.3f out=%.3f\n",
        label,
        static_cast<unsigned long long>(stats.sent_packets),
        static_cast<unsigned long long>(stats.received_packets),
        static_cast<unsigned long long>(stats.decoded_frames),
        static_cast<unsigned long long>(stats.concealed_frames),
        static_cast<unsigned long long>(stats.late_packets),
        static_cast<unsigned long long>(stats.reordered_packets),
        static_cast<unsigned long long>(stats.duplicate_packets),
        static_cast<unsigned long long>(stats.nack_requests),
        static_cast<unsigned long long>(stats.retransmitted_packets),
        static_cast<unsigned long long>(stats.dropped_packets),
        stats.jitter_target_frames,
        stats.jitter_buffered_frames,
        stats.input_level,
        stats.output_level);
}

}  // namespace

int run_voice_loopback(AudioIO& first, AudioIO& second, const LoopbackOptions& options) {
    std::printf("[loopback] two peers, %d s, reorder every %d, drop every %d, AEC %s\n",
                options.seconds, options.reorder_every, options.drop_every,
                options.aec ? "on" : "off");
    const std::uint64_t rss_before = read_rss_kb("VmRSS:");
    const auto started = Clock::now();

    VoiceEngineOptions engine_options;
    engine_options.enable_aec = options.aec;
    engine_options.test_reorder_every = options.reorder_every;
    engine_options.test_drop_every = options.drop_every;

    VoiceEngine a(first, engine_options);
    VoiceEngine b(second, engine_options);

    std::atomic<int> data_messages{0};
    std::mutex data_mutex;
    std::string last_data_message;
    a.set_data_callback([&](const std::string& text) {
        std::lock_guard<std::mutex> lock(data_mutex);
        last_data_message = text;
        ++data_messages;
    });
    b.set_data_callback([&](const std::string& text) {
        std::lock_guard<std::mutex> lock(data_mutex);
        last_data_message = text;
        ++data_messages;
    });

    std::string error;
    std::string offer;
    if (!a.create_offer(&offer, &error)) {
        std::printf("[loopback] FAIL offer: %s\n", error.c_str());
        return 1;
    }
    const auto offer_done = Clock::now();
    std::string answer;
    if (!b.accept_offer(offer, &answer, &error)) {
        std::printf("[loopback] FAIL answer: %s\n", error.c_str());
        return 1;
    }
    if (!a.accept_answer(answer, &error)) {
        std::printf("[loopback] FAIL remote answer: %s\n", error.c_str());
        return 1;
    }
    const auto sdp_done = Clock::now();

    if (!a.start_audio(&error)) {
        std::printf("[loopback] FAIL audio A: %s\n", error.c_str());
        return 1;
    }
    if (!b.start_audio(&error)) {
        std::printf("[loopback] FAIL audio B: %s\n", error.c_str());
        a.stop();
        return 1;
    }

    const auto connect_deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < connect_deadline && !(a.connected() && b.connected())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!a.connected() || !b.connected()) {
        std::printf("[loopback] FAIL connect: a=%d b=%d\n", static_cast<int>(a.peer_state()),
                    static_cast<int>(b.peer_state()));
        print_stats("A", a.stats());
        print_stats("B", b.stats());
        return 1;
    }
    const auto connected_at = Clock::now();
    std::printf("[loopback] connected: offer=%lld ms, sdp=%lld ms, connect=%lld ms\n",
                elapsed_ms(started, offer_done), elapsed_ms(offer_done, sdp_done),
                elapsed_ms(sdp_done, connected_at));

    bool data_sent = false;
    const auto data_deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < data_deadline && data_messages.load() == 0) {
        data_sent = a.send_data("{\"type\":\"loopback.ping\"}") || data_sent;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const auto run_until = Clock::now() + std::chrono::seconds(options.seconds);
    auto next_report = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < run_until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (Clock::now() >= next_report) {
            next_report += std::chrono::seconds(1);
            const VoiceEngineStats stats_a = a.stats();
            const VoiceEngineStats stats_b = b.stats();
            std::printf("[loopback] t=%llds A recv=%llu dec=%llu out=%.3f | B recv=%llu dec=%llu out=%.3f\n",
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - connected_at)
                                .count()),
                        static_cast<unsigned long long>(stats_a.received_packets),
                        static_cast<unsigned long long>(stats_a.decoded_frames), stats_a.output_level,
                        static_cast<unsigned long long>(stats_b.received_packets),
                        static_cast<unsigned long long>(stats_b.decoded_frames), stats_b.output_level);
        }
    }

    const VoiceEngineStats stats_a = a.stats();
    const VoiceEngineStats stats_b = b.stats();
    print_stats("A", stats_a);
    print_stats("B", stats_b);
    const std::uint64_t rss_after = read_rss_kb("VmRSS:");
    const std::uint64_t rss_peak = read_rss_kb("VmHWM:");
    std::printf("[loopback] RSS %llu -> %llu KiB (peak %llu KiB)\n",
                static_cast<unsigned long long>(rss_before),
                static_cast<unsigned long long>(rss_after),
                static_cast<unsigned long long>(rss_peak));
    std::printf("[loopback] jitter delay A=%d ms B=%d ms, data messages=%d\n",
                stats_a.jitter_target_frames * 20, stats_b.jitter_target_frames * 20,
                data_messages.load());

    a.stop();
    b.stop();

    int failures = 0;
    auto check = [&failures](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::printf("[loopback] check failed: %s\n", message);
        }
    };
    check(stats_a.sent_packets > 0, "A sent Opus packets");
    check(stats_b.sent_packets > 0, "B sent Opus packets");
    check(stats_a.received_packets > 0, "A received Opus packets");
    check(stats_b.received_packets > 0, "B received Opus packets");
    check(stats_a.decoded_frames > 0, "A decoded Opus frames");
    check(stats_b.decoded_frames > 0, "B decoded Opus frames");
    check(stats_a.output_level > 0.001f, "A decoded non-silent audio");
    check(stats_b.output_level > 0.001f, "B decoded non-silent audio");
    check(stats_a.reordered_packets > 0 || stats_b.reordered_packets > 0,
          "jitter buffer saw reordered packets");
    check(stats_a.concealed_frames > 0 || stats_b.concealed_frames > 0,
          "jitter buffer concealed dropped packets");
    check(stats_a.nack_requests > 0 || stats_b.nack_requests > 0, "NACK requests were generated");
    check(data_messages.load() > 0, "data channel delivered a message");
    (void)data_sent;
    (void)last_data_message;

    if (failures == 0) {
        std::printf("[loopback] PASS\n");
        return 0;
    }
    std::printf("[loopback] FAIL (%d checks)\n", failures);
    return 1;
}

}  // namespace pk::voice
