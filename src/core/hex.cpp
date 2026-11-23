#include <blackbox/util/hex.hpp>

namespace bb {

std::string to_hex(std::span<const std::byte> data) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.resize(data.size() * 2);
  for (std::size_t i = 0; i < data.size(); ++i) {
    const std::uint8_t byte = std::to_integer<std::uint8_t>(data[i]);
    result[i * 2] = kHex[byte >> 4];
    result[i * 2 + 1] = kHex[byte & 0x0F];
  }
  return result;
}

std::string to_hex(const void* data, std::size_t size) {
  return to_hex(std::span<const std::byte>(static_cast<const std::byte*>(data), size));
}

bool secure_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return difference == 0;
}

}  // namespace bb
