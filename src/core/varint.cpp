#include <blackbox/util/varint.hpp>

#include <bit>
#include <cstring>

namespace bb {

void Encoder::u8(std::uint8_t value) noexcept {
  if (reserve(1)) {
    out_[pos_++] = static_cast<std::byte>(value);
  }
}

void Encoder::u16(std::uint16_t value) noexcept { put_small(value, 2); }

void Encoder::u32(std::uint32_t value) noexcept { put_small(value, 4); }

void Encoder::u64(std::uint64_t value) noexcept { put_small(value, 8); }

void Encoder::put_small(std::uint64_t value, std::size_t bytes) noexcept {
  if (!reserve(bytes)) {
    return;
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    out_[pos_++] = static_cast<std::byte>((value >> (i * 8)) & 0xFFu);
  }
}

void Encoder::varint(std::uint64_t value) noexcept {
  std::uint8_t buffer[10];
  std::size_t length = 0;
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
    value >>= 7;
    if (value != 0) {
      byte |= 0x80u;
    }
    buffer[length++] = byte;
  } while (value != 0);
  bytes(buffer, length);
}

void Encoder::ivarint(std::int64_t value) noexcept {
  // Zigzag: non-negative values become even, negative values odd.
  const std::uint64_t zigzag =
      (static_cast<std::uint64_t>(value) << 1) ^
      static_cast<std::uint64_t>(value >> 63);
  varint(zigzag);
}

void Encoder::f64(double value) noexcept {
  u64(std::bit_cast<std::uint64_t>(value));
}

void Encoder::bytes(const void* data, std::size_t size) noexcept {
  if (size == 0) {
    return;
  }
  if (!reserve(size)) {
    return;
  }
  if (async_signal_safe_) {
    const auto* source = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
      out_[pos_ + i] = static_cast<std::byte>(source[i]);
    }
  } else {
    std::memcpy(out_.data() + pos_, data, size);
  }
  pos_ += size;
}

void Encoder::raw_string(std::string_view text) noexcept {
  varint(text.size());
  bytes(text.data(), text.size());
}

bool Encoder::reserve(std::size_t size) noexcept {
  if (!ok_) {
    return false;
  }
  if (size > out_.size() - pos_) {
    ok_ = false;
    return false;
  }
  return true;
}

std::uint8_t Decoder::u8() noexcept {
  if (!need(1)) {
    return 0;
  }
  return std::to_integer<std::uint8_t>(in_[pos_++]);
}

std::uint16_t Decoder::u16() noexcept {
  if (!need(2)) {
    return 0;
  }
  std::uint16_t value = 0;
  for (std::size_t i = 0; i < 2; ++i) {
    value |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in_[pos_++]))
             << (i * 8);
  }
  return value;
}

std::uint32_t Decoder::u32() noexcept {
  if (!need(4)) {
    return 0;
  }
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in_[pos_++]))
             << (i * 8);
  }
  return value;
}

std::uint64_t Decoder::u64() noexcept {
  if (!need(8)) {
    return 0;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in_[pos_++]))
             << (i * 8);
  }
  return value;
}

std::uint64_t Decoder::varint() noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 10; ++index) {
    if (!need(1)) {
      return 0;
    }
    const std::uint8_t byte = std::to_integer<std::uint8_t>(in_[pos_++]);
    if (index == 9 && (byte & 0xFEu) != 0) {
      ok_ = false;  // overflow beyond 64 bits
      return 0;
    }
    value |= static_cast<std::uint64_t>(byte & 0x7Fu) << (index * 7);
    if ((byte & 0x80u) == 0) {
      return value;
    }
  }
  ok_ = false;  // more than 10 bytes
  return 0;
}

std::int64_t Decoder::ivarint() noexcept {
  const std::uint64_t zigzag = varint();
  if (!ok_) {
    return 0;
  }
  return static_cast<std::int64_t>(zigzag >> 1) ^
         -static_cast<std::int64_t>(zigzag & 1u);
}

double Decoder::f64() noexcept {
  const std::uint64_t bits = u64();
  return std::bit_cast<double>(bits);
}

bool Decoder::bytes(void* out, std::size_t size) noexcept {
  if (!need(size)) {
    return false;
  }
  if (size != 0) {
    std::memcpy(out, in_.data() + pos_, size);
    pos_ += size;
  }
  return true;
}

bool Decoder::skip(std::size_t size) noexcept {
  if (!need(size)) {
    return false;
  }
  pos_ += size;
  return true;
}

bool Decoder::string_into(char* out, std::size_t cap, std::size_t& length) noexcept {
  length = 0;
  if (cap == 0) {
    ok_ = false;
    return false;
  }
  const std::uint64_t encoded = varint();
  if (!ok_ || encoded >= cap) {
    ok_ = false;
    return false;
  }
  if (!need(static_cast<std::size_t>(encoded))) {
    return false;
  }
  if (encoded != 0) {
    std::memcpy(out, in_.data() + pos_, static_cast<std::size_t>(encoded));
    pos_ += static_cast<std::size_t>(encoded);
  }
  out[encoded] = '\0';
  length = static_cast<std::size_t>(encoded);
  return true;
}

bool Decoder::need(std::size_t size) noexcept {
  if (!ok_) {
    return false;
  }
  if (size > in_.size() - pos_) {
    ok_ = false;
    return false;
  }
  return true;
}

}  // namespace bb
