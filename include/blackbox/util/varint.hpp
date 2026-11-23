#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bb {

/// LEB128-style unsigned varint size in bytes (1..10).
constexpr std::size_t uvarint_size(std::uint64_t value) noexcept {
  std::size_t size = 1;
  while (value >= 0x80u) {
    value >>= 7;
    ++size;
  }
  return size;
}

/// Bounded, non-throwing byte encoder. After the first overflow every further
/// write is ignored and ok() becomes false. The output never exceeds the span
/// passed to the constructor.
class Encoder {
 public:
  explicit Encoder(std::span<std::byte> out) noexcept : out_(out) {}

  bool ok() const noexcept { return ok_; }
  std::size_t size() const noexcept { return pos_; }
  std::span<const std::byte> written() const noexcept {
    return out_.first(pos_);
  }

  void u8(std::uint8_t value) noexcept;
  void u16(std::uint16_t value) noexcept;
  void u32(std::uint32_t value) noexcept;
  void u64(std::uint64_t value) noexcept;
  void varint(std::uint64_t value) noexcept;
  void ivarint(std::int64_t value) noexcept;
  void f64(double value) noexcept;
  void bytes(const void* data, std::size_t size) noexcept;
  void raw_string(std::string_view text) noexcept;

 private:
  bool reserve(std::size_t size) noexcept;
  void put_small(std::uint64_t value, std::size_t bytes) noexcept;

  std::span<std::byte> out_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

/// Bounded, non-throwing byte decoder. Reads past the end set ok() to false and
/// return zero. A malformed varint longer than 10 bytes or with overflow bits is
/// rejected. Strings are never truncated: if the encoded length does not fit
/// the caller's buffer the decode fails.
class Decoder {
 public:
  explicit Decoder(std::span<const std::byte> in) noexcept : in_(in) {}

  bool ok() const noexcept { return ok_; }
  void fail() noexcept { ok_ = false; }
  std::size_t position() const noexcept { return pos_; }
  std::size_t remaining() const noexcept { return in_.size() - pos_; }
  bool at_end() const noexcept { return pos_ == in_.size(); }
  std::span<const std::byte> remaining_bytes() const noexcept {
    return in_.subspan(pos_);
  }

  std::uint8_t u8() noexcept;
  std::uint16_t u16() noexcept;
  std::uint32_t u32() noexcept;
  std::uint64_t u64() noexcept;
  std::uint64_t varint() noexcept;
  std::int64_t ivarint() noexcept;
  double f64() noexcept;
  bool bytes(void* out, std::size_t size) noexcept;
  bool skip(std::size_t size) noexcept;
  /// Reads a length-prefixed string into `out`, which must be at least `cap`
  /// bytes. On success returns the text length (without terminator) and writes
  /// a NUL terminator. Fails if the text does not fit in `cap - 1`.
  bool string_into(char* out, std::size_t cap, std::size_t& length) noexcept;

 private:
  bool need(std::size_t size) noexcept;

  std::span<const std::byte> in_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace bb
