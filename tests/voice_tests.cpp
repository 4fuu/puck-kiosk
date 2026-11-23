#include "voice/jitter_buffer.h"
#include "voice/opus_codec.h"
#include "voice/rtp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, const char* file, int line,
           const std::string& detail = {}) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: check failed: %s", file, line, expression);
    if (!detail.empty()) {
        std::fprintf(stderr, " (%s)", detail.c_str());
    }
    std::fputc('\n', stderr);
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define CHECK_MSG(expression, detail) check((expression), #expression, __FILE__, __LINE__, (detail))

pk::voice::RtpPacket make_packet(std::uint16_t sequence, std::uint32_t timestamp,
                                 std::vector<std::uint8_t> payload) {
    pk::voice::RtpPacket packet;
    packet.payload_type = 111;
    packet.sequence = sequence;
    packet.timestamp = timestamp;
    packet.ssrc = 0x12345678;
    packet.payload = std::move(payload);
    return packet;
}

std::vector<std::uint8_t> payload_for(std::uint16_t sequence) {
    return {static_cast<std::uint8_t>(sequence & 0xff),
            static_cast<std::uint8_t>((sequence >> 8) & 0xff), 0xaa, 0x55};
}

void test_rtp_roundtrip() {
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    const auto packet = pk::voice::build_rtp(111, 4242, 0xdeadbeef, 0x11223344, payload.data(),
                                             payload.size());
    CHECK(packet.size() == 12 + payload.size());
    pk::voice::RtpPacket parsed;
    CHECK(pk::voice::parse_rtp(packet.data(), packet.size(), &parsed));
    CHECK(parsed.payload_type == 111);
    CHECK(parsed.sequence == 4242);
    CHECK(parsed.timestamp == 0xdeadbeef);
    CHECK(parsed.ssrc == 0x11223344);
    CHECK(parsed.payload == payload);
    CHECK(!pk::voice::parse_rtp(packet.data(), 4, &parsed));

    // One-byte header extension: profile 0xBEDE, one word of extension data.
    std::vector<std::uint8_t> with_extension = {0x90, 111, 0, 7, 0, 0, 0, 9, 0, 0, 0, 1,
                                                0xbe, 0xde, 0, 1, 0x10, 0xaa, 0xbb, 0xcc,
                                                0x01, 0x02};
    CHECK(pk::voice::parse_rtp(with_extension.data(), with_extension.size(), &parsed));
    CHECK(parsed.sequence == 7);
    CHECK(parsed.payload.size() == 2);
    CHECK(parsed.payload[0] == 0x01 && parsed.payload[1] == 0x02);

    // Padding is stripped using the last byte's count.
    std::vector<std::uint8_t> padded = {0xa0, 111, 0, 8, 0, 0, 0, 1, 0, 0, 0, 1, 0x42, 0x43, 0, 2};
    CHECK(pk::voice::parse_rtp(padded.data(), padded.size(), &parsed));
    CHECK(parsed.payload.size() == 2);
    CHECK(parsed.payload[0] == 0x42 && parsed.payload[1] == 0x43);
}

void test_rtcp_nack_roundtrip() {
    pk::voice::RtcpNack nack;
    nack.sender_ssrc = 0x11111111;
    nack.media_ssrc = 0x22222222;
    nack.sequence_numbers = {100, 101, 102, 110, 120, 121};
    const auto bytes = pk::voice::build_rtcp_nack(nack);
    CHECK(bytes.size() >= 16);
    CHECK(pk::voice::is_rtcp(bytes.data(), bytes.size()));
    CHECK(bytes[1] == 205);

    pk::voice::RtcpNack parsed;
    CHECK(pk::voice::parse_rtcp_nack(bytes.data(), bytes.size(), &parsed));
    CHECK(parsed.sender_ssrc == nack.sender_ssrc);
    CHECK(parsed.media_ssrc == nack.media_ssrc);
    CHECK(parsed.sequence_numbers == nack.sequence_numbers);

    std::vector<std::uint8_t> rtp = pk::voice::build_rtp(111, 1, 0, 0, nullptr, 0);
    CHECK(!pk::voice::is_rtcp(rtp.data(), rtp.size()));
}

void test_jitter_in_order() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 3;
    config.min_delay_frames = 1;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Empty);
    for (std::uint16_t i = 2; i < 5; ++i) {
        CHECK(jitter.push(make_packet(i, i * 960, payload_for(i))));
    }
    for (std::uint16_t i = 0; i < 5; ++i) {
        const auto frame = jitter.pop();
        CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
        CHECK(frame.payload == payload_for(i));
        CHECK(frame.timestamp == i * 960);
    }
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Empty);
    CHECK(jitter.stats().played == 5);
}

void test_jitter_reorder() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 3;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.push(make_packet(2, 1920, payload_for(2))));
    CHECK(jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.push(make_packet(4, 3840, payload_for(4))));
    CHECK(jitter.push(make_packet(3, 2880, payload_for(3))));
    CHECK(jitter.stats().reordered == 2);
    for (std::uint16_t i = 0; i < 5; ++i) {
        const auto frame = jitter.pop();
        CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
        CHECK(frame.payload == payload_for(i));
    }
}

void test_jitter_loss() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 2;
    config.min_delay_frames = 1;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.push(make_packet(3, 2880, payload_for(3))));
    CHECK(jitter.push(make_packet(4, 3840, payload_for(4))));
    CHECK(jitter.push(make_packet(5, 4800, payload_for(5))));

    auto frame = jitter.pop();
    CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(frame.payload == payload_for(0));
    frame = jitter.pop();
    CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(frame.payload == payload_for(1));

    const auto missing = jitter.take_nack_requests();
    CHECK(std::find(missing.begin(), missing.end(), 2) != missing.end());
    frame = jitter.pop();
    CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Concealed);
    CHECK(jitter.stats().concealed == 1);

    for (std::uint16_t i = 3; i <= 5; ++i) {
        frame = jitter.pop();
        CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
        CHECK(frame.payload == payload_for(i));
    }
}

void test_jitter_duplicates_and_late() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 1;
    config.min_delay_frames = 1;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(!jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.stats().duplicates == 1);
    CHECK(jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(!jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(!jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.stats().late == 2);
}

void test_jitter_adapts() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 2;
    config.min_delay_frames = 1;
    config.max_delay_frames = 8;
    config.decrease_after_frames = 40;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.target_delay_frames() == 2);

    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);

    CHECK(jitter.push(make_packet(3, 2880, payload_for(3))));
    CHECK(jitter.push(make_packet(4, 3840, payload_for(4))));
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Concealed);
    CHECK(jitter.target_delay_frames() == 2);
    CHECK(jitter.stats().target_increases == 0);

    // A packet arriving after playout passed it is jitter, so it raises the
    // target; a lost packet alone does not.
    CHECK(!jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.stats().late == 1);
    CHECK(jitter.target_delay_frames() == 3);
    CHECK(jitter.stats().target_increases == 1);

    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);

    std::uint16_t sequence = 5;
    std::uint32_t timestamp = 4800;
    for (int i = 0; i < 120; ++i) {
        CHECK(jitter.push(make_packet(sequence, timestamp, payload_for(sequence))));
        const auto frame = jitter.pop();
        CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
        ++sequence;
        timestamp += 960;
    }
    CHECK(jitter.target_delay_frames() < 3);
    CHECK(jitter.stats().target_decreases > 0);
}

void test_jitter_sequence_wrap() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 2;
    config.min_delay_frames = 1;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(65534, 0, payload_for(65534))));
    CHECK(jitter.push(make_packet(65535, 960, payload_for(65535))));
    CHECK(jitter.push(make_packet(0, 1920, payload_for(0))));
    CHECK(jitter.push(make_packet(1, 2880, payload_for(1))));
    for (std::uint16_t i = 0; i < 4; ++i) {
        const std::uint16_t expected = static_cast<std::uint16_t>(65534 + i);
        const auto frame = jitter.pop();
        CHECK(frame.kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
        CHECK(frame.payload == payload_for(expected));
    }
}

void test_jitter_nack_retry() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 3;
    config.nack_retry_rounds = 2;
    config.nack_max_requests = 2;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.push(make_packet(2, 1920, payload_for(2))));
    CHECK(jitter.push(make_packet(3, 2880, payload_for(3))));

    auto first = jitter.take_nack_requests();
    CHECK(std::find(first.begin(), first.end(), 1) != first.end());
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(jitter.take_nack_requests().empty());
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Empty);
    auto retry = jitter.take_nack_requests();
    CHECK(std::find(retry.begin(), retry.end(), 1) != retry.end());
    CHECK(jitter.take_nack_requests().empty());
    CHECK(jitter.stats().nack_requests == 2);
}

double correlation_at_lag(const std::vector<std::int16_t>& reference,
                          const std::vector<std::int16_t>& decoded, int lag) {
    double dot = 0.0;
    double ref_energy = 0.0;
    double dec_energy = 0.0;
    for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < decoded.size(); ++i) {
        const double a = reference[i];
        const double b = decoded[i + static_cast<std::size_t>(lag)];
        dot += a * b;
        ref_energy += a * a;
        dec_energy += b * b;
    }
    if (ref_energy <= 0.0 || dec_energy <= 0.0) {
        return 0.0;
    }
    return dot / std::sqrt(ref_energy * dec_energy);
}

void test_opus_roundtrip() {
    pk::voice::OpusEncoder encoder;
    pk::voice::OpusDecoder decoder;
    std::string error;
    CHECK_MSG(encoder.initialize(48000, 1, 32000, &error), error);
    CHECK_MSG(decoder.initialize(48000, 1, &error), error);

    constexpr int kFrame = 960;
    std::vector<std::int16_t> input(static_cast<std::size_t>(kFrame) * 50);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        input[i] = static_cast<std::int16_t>(0.3 * 32767.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
    }

    std::vector<std::int16_t> decoded(input.size(), 0);
    std::vector<std::uint8_t> payload;
    int encoded_frames = 0;
    for (int frame = 0; frame < 50; ++frame) {
        const int size = encoder.encode(input.data() + frame * kFrame, kFrame, &payload, &error);
        CHECK_MSG(size > 0, error);
        if (size <= 0) {
            return;
        }
        const int samples =
            decoder.decode(payload.data(), payload.size(), decoded.data() + frame * kFrame, kFrame,
                           &error);
        CHECK_MSG(samples == kFrame, error);
        ++encoded_frames;
    }
    CHECK(encoded_frames == 50);

    double best = 0.0;
    for (int lag = 0; lag <= 400; ++lag) {
        best = std::max(best, correlation_at_lag(input, decoded, lag));
    }
    CHECK_MSG(best > 0.8, "correlation " + std::to_string(best));

    std::vector<std::int16_t> plc(kFrame, 0);
    CHECK(decoder.decode_plc(plc.data(), kFrame, &error) == kFrame);
}

void test_jitter_stats_are_consistent() {
    pk::voice::ReceiveJitterBuffer::Config config;
    config.initial_delay_frames = 2;
    pk::voice::ReceiveJitterBuffer jitter(config);
    CHECK(jitter.push(make_packet(0, 0, payload_for(0))));
    CHECK(jitter.push(make_packet(1, 960, payload_for(1))));
    CHECK(jitter.push(make_packet(2, 1920, payload_for(2))));
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    CHECK(jitter.pop().kind == pk::voice::ReceiveJitterBuffer::PopKind::Frame);
    const auto stats = jitter.stats();
    CHECK(stats.received == 3);
    CHECK(stats.played == 3);
    CHECK(stats.concealed == 0);
}

struct TestCase {
    const char* name;
    void (*function)();
};

}  // namespace

int main() {
    const TestCase tests[] = {
        {"rtp roundtrip and extension", test_rtp_roundtrip},
        {"rtcp nack roundtrip", test_rtcp_nack_roundtrip},
        {"jitter in order", test_jitter_in_order},
        {"jitter reorder", test_jitter_reorder},
        {"jitter loss and nack", test_jitter_loss},
        {"jitter duplicates and late", test_jitter_duplicates_and_late},
        {"jitter adaptive delay", test_jitter_adapts},
        {"jitter sequence wrap", test_jitter_sequence_wrap},
        {"jitter nack retry", test_jitter_nack_retry},
        {"jitter stats", test_jitter_stats_are_consistent},
        {"opus roundtrip", test_opus_roundtrip},
    };

    std::printf("[==========] voice-tests\n");
    for (const TestCase& test : tests) {
        const int failures_before = g_failures;
        std::printf("[ RUN      ] %s\n", test.name);
        test.function();
        if (g_failures == failures_before) {
            std::printf("[       OK ] %s\n", test.name);
        } else {
            std::printf("[   FAILED ] %s\n", test.name);
        }
    }

    if (g_failures == 0) {
        std::printf("[  PASSED  ] %d checks\n", g_checks);
        return 0;
    }
    std::printf("[  FAILED  ] %d of %d checks\n", g_failures, g_checks);
    return 1;
}
