#include "voice/rtp.h"

#include <algorithm>
#include <cstring>

namespace pk::voice {
namespace {

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t read_u32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

void write_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void write_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

}  // namespace

bool parse_rtp(const std::uint8_t* data, std::size_t size, RtpPacket* packet) {
    if (!data || !packet || size < 12) {
        return false;
    }
    if (((data[0] >> 6) & 0x3) != 2) {
        return false;
    }

    const bool padding = (data[0] & 0x20) != 0;
    const bool extension = (data[0] & 0x10) != 0;
    const std::size_t csrc_count = data[0] & 0x0f;
    std::size_t header_size = 12 + csrc_count * 4;
    if (size < header_size) {
        return false;
    }
    if (extension) {
        if (size < header_size + 4) {
            return false;
        }
        const std::size_t extension_words = read_u16(data + header_size + 2);
        header_size += 4 + extension_words * 4;
        if (size < header_size) {
            return false;
        }
    }

    std::size_t payload_end = size;
    if (padding) {
        const std::size_t padding_size = data[size - 1];
        if (padding_size == 0 || padding_size > size - header_size) {
            return false;
        }
        payload_end -= padding_size;
    }

    packet->payload_type = data[1] & 0x7f;
    packet->marker = (data[1] & 0x80) != 0;
    packet->sequence = read_u16(data + 2);
    packet->timestamp = read_u32(data + 4);
    packet->ssrc = read_u32(data + 8);
    packet->payload.assign(data + header_size, data + payload_end);
    return true;
}

std::vector<std::uint8_t> build_rtp(std::uint8_t payload_type, std::uint16_t sequence,
                                    std::uint32_t timestamp, std::uint32_t ssrc,
                                    const std::uint8_t* payload, std::size_t size) {
    std::vector<std::uint8_t> packet;
    packet.reserve(12 + size);
    packet.push_back(0x80);
    packet.push_back(static_cast<std::uint8_t>(payload_type & 0x7f));
    write_u16(&packet, sequence);
    write_u32(&packet, timestamp);
    write_u32(&packet, ssrc);
    if (payload && size > 0) {
        packet.insert(packet.end(), payload, payload + size);
    }
    return packet;
}

bool is_rtcp(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 2 || ((data[0] >> 6) & 0x3) != 2) {
        return false;
    }
    const std::uint8_t payload_type = data[1];
    return payload_type >= 192 && payload_type <= 223;
}

std::vector<std::uint8_t> build_rtcp_nack(const RtcpNack& nack) {
    std::vector<std::uint16_t> sequence_numbers = nack.sequence_numbers;
    std::sort(sequence_numbers.begin(), sequence_numbers.end());
    sequence_numbers.erase(std::unique(sequence_numbers.begin(), sequence_numbers.end()),
                           sequence_numbers.end());

    struct Field {
        std::uint16_t pid = 0;
        std::uint16_t blp = 0;
    };
    std::vector<Field> fields;
    for (std::uint16_t sequence : sequence_numbers) {
        if (fields.empty() || sequence <= fields.back().pid ||
            sequence - fields.back().pid > 16) {
            fields.push_back(Field{sequence, 0});
            continue;
        }
        const std::uint16_t offset = static_cast<std::uint16_t>(sequence - fields.back().pid);
        fields.back().blp = static_cast<std::uint16_t>(fields.back().blp | (1u << (offset - 1)));
    }

    const std::size_t words = 3 + fields.size();
    std::vector<std::uint8_t> packet;
    packet.reserve(words * 4);
    packet.push_back(0x81);  // version 2, FMT 1 (generic NACK), no padding
    packet.push_back(205);   // RTCP payload type 205
    write_u16(&packet, static_cast<std::uint16_t>(words - 1));
    write_u32(&packet, nack.sender_ssrc);
    write_u32(&packet, nack.media_ssrc);
    for (const Field& field : fields) {
        write_u16(&packet, field.pid);
        write_u16(&packet, field.blp);
    }
    return packet;
}

bool parse_rtcp_nack(const std::uint8_t* data, std::size_t size, RtcpNack* nack) {
    if (!data || !nack) {
        return false;
    }
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        if (((data[offset] >> 6) & 0x3) != 2) {
            return false;
        }
        const std::uint8_t payload_type = data[offset + 1];
        const std::size_t words = static_cast<std::size_t>(read_u16(data + offset + 2)) + 1;
        const std::size_t packet_size = words * 4;
        if (packet_size < 4 || offset + packet_size > size) {
            return false;
        }
        const std::uint8_t format = data[offset] & 0x1f;
        if (payload_type == 205 && format == 1) {
            if (packet_size < 12) {
                return false;
            }
            nack->sender_ssrc = read_u32(data + offset + 4);
            nack->media_ssrc = read_u32(data + offset + 8);
            nack->sequence_numbers.clear();
            for (std::size_t field = 12; field + 4 <= packet_size; field += 4) {
                const std::uint16_t pid = read_u16(data + offset + field);
                const std::uint16_t blp = read_u16(data + offset + field + 2);
                nack->sequence_numbers.push_back(pid);
                for (int bit = 0; bit < 16; ++bit) {
                    if ((blp & (1u << bit)) != 0) {
                        nack->sequence_numbers.push_back(static_cast<std::uint16_t>(pid + bit + 1));
                    }
                }
            }
            return true;
        }
        offset += packet_size;
    }
    return false;
}

}  // namespace pk::voice
