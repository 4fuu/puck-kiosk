#include "core/sha256.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace pk {
namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

inline std::uint32_t rotate_right(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0f]);
    }
    return hex;
}

}  // namespace

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    buffer_.fill(0);
    buffer_size_ = 0;
    total_bytes_ = 0;
}

void Sha256::update(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    total_bytes_ += size;
    while (size > 0) {
        const std::size_t take = std::min(size, buffer_.size() - buffer_size_);
        std::memcpy(buffer_.data() + buffer_size_, bytes, take);
        buffer_size_ += take;
        bytes += take;
        size -= take;
        if (buffer_size_ == buffer_.size()) {
            transform(buffer_.data());
            buffer_size_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finish() {
    const std::uint64_t bit_count = total_bytes_ * 8;
    buffer_[buffer_size_++] = 0x80;
    if (buffer_size_ > 56) {
        std::memset(buffer_.data() + buffer_size_, 0, buffer_.size() - buffer_size_);
        transform(buffer_.data());
        buffer_size_ = 0;
    }
    std::memset(buffer_.data() + buffer_size_, 0, 56 - buffer_size_);
    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<std::uint8_t>(bit_count >> (56 - 8 * i));
    }
    transform(buffer_.data());

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        digest[i * 4 + 0] = static_cast<std::uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
}

void Sha256::transform(const std::uint8_t* block) {
    std::uint32_t schedule[64];
    for (int i = 0; i < 16; ++i) {
        schedule[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                                 rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                                 rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choice + kRound[i] + schedule[i];
        const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size) {
    Sha256 hash;
    hash.update(data, size);
    return hash.finish();
}

std::string sha256_hex(const void* data, std::size_t size) {
    return to_hex(sha256(data, size));
}

std::string sha256_hex(const std::string& data) {
    return sha256_hex(data.data(), data.size());
}

bool sha256_file(const std::string& path, std::string* hex, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) {
            *error = "cannot open " + path;
        }
        return false;
    }
    Sha256 hash;
    std::vector<char> chunk(1024 * 1024);
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize read = input.gcount();
        if (read > 0) {
            hash.update(chunk.data(), static_cast<std::size_t>(read));
        }
    }
    if (!input.eof()) {
        if (error) {
            *error = "cannot read " + path;
        }
        return false;
    }
    if (hex) {
        *hex = to_hex(hash.finish());
    }
    return true;
}

}  // namespace pk
