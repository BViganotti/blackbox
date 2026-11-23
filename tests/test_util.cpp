#include "harness.hpp"

#include <blackbox/util/crc32c.hpp>
#include <blackbox/util/hex.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>
#include <blackbox/util/varint.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

BB_TEST(crc32c_known_vector) {
  const std::string text = "123456789";
  CHECK_EQ(bb::crc32c(reinterpret_cast<const std::byte*>(text.data()),
                      text.size()),
           0xE3069283u);
}

BB_TEST(crc32c_empty_and_incremental) {
  CHECK_EQ(bb::crc32c(static_cast<const std::byte*>(nullptr), 0), 0u);
  const std::string text = "the quick brown fox jumps over the lazy dog";
  bb::Crc32c incremental;
  incremental.update(text.data(), 10);
  incremental.update(text.data() + 10, text.size() - 10);
  CHECK_EQ(incremental.value(),
           bb::crc32c(reinterpret_cast<const std::byte*>(text.data()),
                      text.size()));
}

BB_TEST(varint_roundtrip_boundaries) {
  const std::array<std::uint64_t, 9> values = {
      0,          1,          127,        128,        300,
      16383,      16384,      1ull << 32, std::numeric_limits<std::uint64_t>::max()};
  for (std::uint64_t value : values) {
    std::array<std::byte, 16> buffer{};
    bb::Encoder encoder(buffer);
    encoder.varint(value);
    CHECK(encoder.ok());
    CHECK_EQ(encoder.size(), bb::uvarint_size(value));
    bb::Decoder decoder(encoder.written());
    CHECK_EQ(decoder.varint(), value);
    CHECK(decoder.ok());
    CHECK(decoder.at_end());
  }
}

BB_TEST(varint_rejects_oversized) {
  const std::array<std::byte, 11> eleven = {
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}};
  bb::Decoder decoder(eleven);
  (void)decoder.varint();
  CHECK(!decoder.ok());
}

BB_TEST(varint_rejects_overflow_bit) {
  std::array<std::byte, 10> buffer{};
  for (int i = 0; i < 9; ++i) {
    buffer[static_cast<std::size_t>(i)] = std::byte{0x80};
  }
  buffer[9] = std::byte{0x02};
  bb::Decoder decoder(buffer);
  (void)decoder.varint();
  CHECK(!decoder.ok());
}

BB_TEST(signed_varint_roundtrip) {
  const std::array<std::int64_t, 8> values = {
      0, 1, -1, 63, -64, 1'000'000, -1'000'000,
      std::numeric_limits<std::int64_t>::min()};
  for (std::int64_t value : values) {
    std::array<std::byte, 16> buffer{};
    bb::Encoder encoder(buffer);
    encoder.ivarint(value);
    CHECK(encoder.ok());
    bb::Decoder decoder(encoder.written());
    CHECK_EQ(decoder.ivarint(), value);
    CHECK(decoder.ok());
  }
}

BB_TEST(encoder_signal_safe_mode_matches_normal_mode) {
  const std::string text = "signal-safe string with bytes";
  std::array<std::byte, 256> normal_buffer{};
  std::array<std::byte, 256> safe_buffer{};
  bb::Encoder normal(normal_buffer);
  bb::Encoder safe(safe_buffer, true);
  normal.raw_string(text);
  safe.raw_string(text);
  normal.ivarint(-12345);
  safe.ivarint(-12345);
  CHECK(normal.ok());
  CHECK(safe.ok());
  CHECK_EQ(normal.size(), safe.size());
  CHECK(std::equal(normal.written().begin(), normal.written().end(),
                   safe.written().begin()));
}

BB_TEST(encoder_reports_overflow_without_writing) {
  std::array<std::byte, 3> buffer{};
  bb::Encoder encoder(buffer);
  encoder.u32(0x01020304);
  CHECK(!encoder.ok());
  CHECK_EQ(encoder.size(), 0u);
  encoder.u8(1);
  CHECK_EQ(encoder.size(), 0u);
}

BB_TEST(decoder_rejects_truncated_and_oversized_strings) {
  std::array<std::byte, 8> buffer{};
  bb::Encoder encoder(buffer);
  encoder.varint(10);
  encoder.bytes("abc", 3);
  CHECK(encoder.ok());
  bb::Decoder decoder(encoder.written());
  char small[4];
  std::size_t length = 0;
  CHECK(!decoder.string_into(small, sizeof(small), length));
  CHECK(!decoder.ok());

  char large[16];
  bb::Decoder second(encoder.written());
  CHECK(!second.string_into(large, sizeof(large), length));
  CHECK(!second.ok());  // declared ten bytes but only three follow
}

BB_TEST(fixed_width_values_roundtrip) {
  std::array<std::byte, 64> buffer{};
  bb::Encoder encoder(buffer);
  encoder.u8(0xAB);
  encoder.u16(0xBEEF);
  encoder.u32(0xDEADBEEF);
  encoder.u64(0x0123456789ABCDEFull);
  encoder.f64(3.5);
  CHECK(encoder.ok());
  bb::Decoder decoder(encoder.written());
  CHECK_EQ(decoder.u8(), 0xAB);
  CHECK_EQ(decoder.u16(), 0xBEEF);
  CHECK_EQ(decoder.u32(), 0xDEADBEEFu);
  CHECK_EQ(decoder.u64(), 0x0123456789ABCDEFull);
  CHECK_EQ(decoder.f64(), 3.5);
  CHECK(decoder.ok());
}

BB_TEST(time_format_epoch) {
  CHECK_EQ(bb::format_wall_ns(0), std::string("1970-01-01T00:00:00.000Z"));
  CHECK_EQ(bb::format_wall_ns(1'000'000'000),
           std::string("1970-01-01T00:00:01.000Z"));
  CHECK_EQ(bb::format_wall_ns(1'234'567'890),
           std::string("1970-01-01T00:00:01.234Z"));
  CHECK_EQ(bb::format_wall_ns(-1),
           std::string("1969-12-31T23:59:59.999Z"));
}

BB_TEST(time_parse_roundtrip) {
  const std::array<std::int64_t, 5> samples = {
      0, 1'700'000'000'123'000'000, 1'234'567'890'000'000,
      -86'400'000'000'000, 253'402'300'799'000'000};
  for (std::int64_t value : samples) {
    const auto parsed = bb::parse_wall_ns(bb::format_wall_ns(value));
    CHECK(parsed.has_value());
    if (parsed) {
      // Formatting has millisecond precision, so compare at that precision.
      const std::int64_t truncated = value - value % 1'000'000;
      const std::int64_t expected =
          value < 0 && value % 1'000'000 != 0 ? truncated - 1'000'000 : truncated;
      CHECK_EQ(*parsed, expected);
    }
  }
}

BB_TEST(time_parse_rejects_malformed) {
  CHECK(!bb::parse_wall_ns("").has_value());
  CHECK(!bb::parse_wall_ns("2026-11-23").has_value());
  CHECK(!bb::parse_wall_ns("2023-02-29T00:00:00Z").has_value());
  CHECK(bb::parse_wall_ns("2024-02-29T00:00:00Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-13-01T00:00:00Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-00-01T00:00:00Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-01-01T24:00:00Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-01-01T00:60:00Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-01-01T00:00:60Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-01-01T00:00:00").has_value());
  CHECK(!bb::parse_wall_ns("2026-01-01T00:00:00.12Z").has_value());
  CHECK(!bb::parse_wall_ns("2026-01-01X00:00:00Z").has_value());
  CHECK(!bb::parse_wall_ns("abcd-ef-ghTij:kl:mnZ").has_value());
}

BB_TEST(duration_formatting) {
  CHECK_EQ(bb::format_duration_ns(0), std::string("0ns"));
  CHECK_EQ(bb::format_duration_ns(999), std::string("999ns"));
  CHECK_EQ(bb::format_duration_ns(1'500), std::string("1.5us"));
  CHECK_EQ(bb::format_duration_ns(1'234'000), std::string("1.234ms"));
  CHECK_EQ(bb::format_duration_ns(2'000'000'000), std::string("2s"));
  CHECK_EQ(bb::format_duration_ns(-1'000'000), std::string("-1ms"));
}

BB_TEST(random_tokens_are_hex_and_sized) {
  const std::string token = bb::random_token(16);
  CHECK_EQ(token.size(), 32u);
  for (char c : token) {
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
  CHECK_NE(bb::random_token(16), token);
}

BB_TEST(xoshiro_is_deterministic) {
  bb::Xoshiro128 first(42);
  bb::Xoshiro128 second(42);
  for (int i = 0; i < 16; ++i) {
    CHECK_EQ(first.next_u32(), second.next_u32());
  }
  bb::Xoshiro128 third(43);
  CHECK_NE(first.next_u64(), third.next_u64());
}

BB_TEST(hex_encoding) {
  const char raw[] = {'\x00', '\x01', '\xfe', '\xff'};
  CHECK_EQ(bb::to_hex(raw, sizeof(raw)), std::string("0001feff"));
  CHECK_EQ(bb::to_hex(raw, 0), std::string(""));
}

BB_TEST(secure_equal_behaves_like_equal) {
  CHECK(bb::secure_equal("abc", "abc"));
  CHECK(!bb::secure_equal("abc", "abd"));
  CHECK(!bb::secure_equal("abc", "ab"));
}
