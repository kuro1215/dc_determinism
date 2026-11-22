// SHA-256 (FIPS 180-4). Same implementation as src/worker/worker.cpp, copied here because the
// source hash of the worker is its identity and must not change.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dc_determinism {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t size);
    void update(const std::string& text) { update(text.data(), text.size()); }

    // Finishes the computation and resets the object (lower-case hex / raw bytes).
    std::string finish_hex();
    std::array<std::uint8_t, 32> finish_digest();

private:
    void block(const std::uint8_t* p);

    std::uint32_t h_[8];
    std::uint64_t length_;
    std::uint8_t buf_[64];
    std::size_t fill_;
};

std::string sha256_hex(const std::string& data);
std::string to_hex(const std::uint8_t* data, std::size_t size);

}  // namespace dc_determinism
