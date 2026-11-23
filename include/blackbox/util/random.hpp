#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bb {

/// Fills `data` with cryptographically strong random bytes.
/// Thread-safe. Returns false if the operating system source fails.
bool fill_random(void* data, std::size_t size) noexcept;

/// Returns a random 64-bit value. Aborts the process if the OS source is
/// unavailable; this is only used for non-security identifiers.
std::uint64_t random_u64() noexcept;

/// Returns `bytes` random bytes encoded as lowercase hexadecimal.
/// Throws std::runtime_error if the OS source fails. `bytes` is clamped to
/// [1, 64].
std::string random_token(std::size_t bytes = 16);

/// Small deterministic generator for tests and fuzzing. Not secure.
/// Implements xoshiro128** with a splitmix64-seeded state.
class Xoshiro128 {
 public:
  explicit Xoshiro128(std::uint64_t seed) noexcept;
  std::uint32_t next_u32() noexcept;
  std::uint64_t next_u64() noexcept;
  std::uint32_t bounded(std::uint32_t bound) noexcept;

 private:
  std::uint32_t state_[4];
};

}  // namespace bb
