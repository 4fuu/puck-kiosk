#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pk::voice {

// One Opus frame as it arrived over RTP. The receive jitter buffer orders these
// by sequence number; timestamps are kept for diagnostics and concealment.
struct RtpPacket {
    std::uint8_t payload_type = 0;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    bool marker = false;
    std::vector<std::uint8_t> payload;
};

// Parses a complete RTP packet, including CSRCs, a one- or two-byte header
// extension, and padding. Returns false for truncated or non-RTP input.
bool parse_rtp(const std::uint8_t* data, std::size_t size, RtpPacket* packet);

std::vector<std::uint8_t> build_rtp(std::uint8_t payload_type, std::uint16_t sequence,
                                    std::uint32_t timestamp, std::uint32_t ssrc,
                                    const std::uint8_t* payload, std::size_t size);

// RTCP and RTP share the same port. Payload types 192-223 identify RTCP.
bool is_rtcp(const std::uint8_t* data, std::size_t size);

struct RtcpNack {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
    std::vector<std::uint16_t> sequence_numbers;
};

std::vector<std::uint8_t> build_rtcp_nack(const RtcpNack& nack);

// Parses the first generic NACK (PT 205, FMT 1) in a possibly compound RTCP
// packet. Returns false when no NACK is present or the packet is malformed.
bool parse_rtcp_nack(const std::uint8_t* data, std::size_t size, RtcpNack* nack);

}  // namespace pk::voice
