#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bb {

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (0x82F63B78u ^ (value >> 1)) : (value >> 1);
    }
    table[i] = value;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable =
    make_crc32c_table();

}  // namespace detail

/// CRC-32C (Castagnoli) over `data`. The initial value is complemented and the
/// result is complemented, matching the standard CRC-32C definition.
constexpr std::uint32_t crc32c(const std::byte* data, std::size_t size,
                               std::uint32_t seed = 0) noexcept {
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t byte = std::to_integer<std::uint8_t>(data[i]);
    crc = detail::kCrc32cTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

inline std::uint32_t crc32c(std::span<const std::byte> data,
                            std::uint32_t seed = 0) noexcept {
  return crc32c(data.data(), data.size(), seed);
}


/// Incremental CRC-32C. The default constructed object produces the same result
/// as the one-shot function for the concatenated input.
class Crc32c {
 public:
  Crc32c() noexcept = default;

  void update(const void* data, std::size_t size) noexcept {
    crc_ = crc32c(static_cast<const std::byte*>(data), size, crc_);
  }
  void update(std::span<const std::byte> data) noexcept {
    update(data.data(), data.size());
  }
  std::uint32_t value() const noexcept { return crc_; }
  void reset() noexcept { crc_ = 0; }

 private:
  std::uint32_t crc_ = 0;
};

}  // namespace bb
