#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pk {

// Self-contained SHA-256. The asset cache verifies content hashes and the
// Linux build does not link OpenSSL for this; keeping it in core also makes it
// available to the test targets.
class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 32> finish();

private:
    void reset();
    void transform(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);
std::string sha256_hex(const void* data, std::size_t size);
std::string sha256_hex(const std::string& data);
bool sha256_file(const std::string& path, std::string* hex, std::string* error);

}  // namespace pk
