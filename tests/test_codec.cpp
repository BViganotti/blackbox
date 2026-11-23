#include "harness.hpp"

#include <blackbox/codec.hpp>
#include <blackbox/string_table.hpp>
#include <blackbox/util/hex.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {

bb::Record rich_record() {
  bb::Record record{};
  record.kind = bb::RecordKind::Error;
  record.level = bb::Level::Error;
  record.status = bb::Status::Timeout;
  record.seq = 42;
  record.wall_ns = 1'700'000'000'123'456'789;
  record.mono_ns = 987'654'321;
  record.duration_ns = 250'000'000;
  record.int_value = -17;
  record.double_value = 2.5;
  record.value = bb::StoredValue{};
  record.trace = bb::TraceId{0x0102030405060708ull, 0x090A0B0C0D0E0F10ull};
  record.span = 3;
  record.parent = 2;
  record.correlation = 0xAABBCCDDull;
  record.cause_seq = 41;
  CHECK(bb::set_record_name(record, "db.timeout"));
  CHECK(bb::set_record_message(record, "database did not answer"));
  CHECK(bb::store_value(bb::Value::string("metric-value"), record.value));

  record.attr_count = 5;
  bb::store_key("http.status", record.attrs[0]);
  CHECK(bb::store_value(bb::Value::integer(500), record.attrs[0].value));
  bb::store_key("db.system", record.attrs[1]);
  CHECK(bb::store_value(bb::Value::string("postgres"), record.attrs[1].value));
  bb::store_key("retry", record.attrs[2]);
  CHECK(bb::store_value(bb::Value::boolean(true), record.attrs[2].value));
  bb::store_key("ratio", record.attrs[3]);
  CHECK(bb::store_value(bb::Value::floating(0.25), record.attrs[3].value));
  bb::store_key("none", record.attrs[4]);
  CHECK(bb::store_value(bb::Value::null(), record.attrs[4].value));
  return record;
}

bb::Record state_record() {
  bb::Record record{};
  record.kind = bb::RecordKind::StateChange;
  record.level = bb::Level::Warn;
  record.seq = 9;
  record.trace = bb::TraceId{1, 2};
  record.span = 5;
  record.correlation = 7;
  record.cause_seq = 8;
  CHECK(bb::set_record_state_key(record, "circuit.breaker"));
  CHECK(bb::store_value(bb::Value::string("closed"), record.state_old));
  CHECK(bb::store_value(bb::Value::string("open"), record.state_new));
  CHECK(bb::set_record_message(record, "failure threshold reached"));
  return record;
}

std::vector<std::byte> encode(const bb::Record& record, bb::StringTable* table) {
  std::array<std::byte, bb::kMaxEncodedRecordBytes> buffer{};
  std::size_t written = 0;
  CHECK(bb::encode_record_to(record, buffer, table, written));
  CHECK(written > 0);
  return std::vector<std::byte>(buffer.data(), buffer.data() + written);
}

void check_record_equal(const bb::Record& a, const bb::Record& b) {
  CHECK_EQ(a.seq, b.seq);
  CHECK_EQ(a.wall_ns, b.wall_ns);
  CHECK_EQ(a.mono_ns, b.mono_ns);
  CHECK_EQ(a.duration_ns, b.duration_ns);
  CHECK_EQ(a.int_value, b.int_value);
  CHECK_EQ(a.double_value, b.double_value);
  CHECK(a.trace == b.trace);
  CHECK_EQ(a.span, b.span);
  CHECK_EQ(a.parent, b.parent);
  CHECK_EQ(a.correlation, b.correlation);
  CHECK_EQ(a.cause_seq, b.cause_seq);
  CHECK_EQ(static_cast<int>(a.kind), static_cast<int>(b.kind));
  CHECK_EQ(static_cast<int>(a.level), static_cast<int>(b.level));
  CHECK_EQ(static_cast<int>(a.status), static_cast<int>(b.status));
  CHECK_EQ(bb::record_name(a), bb::record_name(b));
  CHECK_EQ(bb::record_message(a), bb::record_message(b));
  CHECK_EQ(bb::record_state_key(a), bb::record_state_key(b));
  CHECK(bb::owned_value(a.value) == bb::owned_value(b.value));
  CHECK(bb::owned_value(a.state_old) == bb::owned_value(b.state_old));
  CHECK(bb::owned_value(a.state_new) == bb::owned_value(b.state_new));
  CHECK_EQ(static_cast<int>(a.attr_count), static_cast<int>(b.attr_count));
  for (std::size_t i = 0; i < a.attr_count; ++i) {
    CHECK_EQ(bb::stored_key(a.attrs[i]), bb::stored_key(b.attrs[i]));
    CHECK(bb::owned_value(a.attrs[i].value) == bb::owned_value(b.attrs[i].value));
  }
}

}  // namespace

BB_TEST(string_table_dedups_and_bounds) {
  bb::StringTable table;
  const auto first = table.intern("hello");
  const auto second = table.intern("hello");
  CHECK(first != bb::StringTable::kInvalidId);
  CHECK_EQ(first, second);
  CHECK_EQ(table.size(), 1u);
  std::string_view view;
  CHECK(table.get(first, view));
  CHECK_EQ(view, std::string_view("hello"));
  CHECK(!table.get(99, view));
  CHECK(!table.get(bb::StringTable::kInvalidId, view));
  CHECK_EQ(table.intern(std::string(bb::StringTable::kMaxStringBytes + 1, 'x')),
           bb::StringTable::kInvalidId);
  table.clear();
  CHECK_EQ(table.size(), 0u);
}

BB_TEST(string_table_capacity_is_enforced) {
  bb::StringTable table;
  for (std::size_t i = 0; i < bb::StringTable::kMaxStrings; ++i) {
    CHECK(table.intern("s" + std::to_string(i)) != bb::StringTable::kInvalidId);
  }
  CHECK_EQ(table.size(), bb::StringTable::kMaxStrings);
  CHECK_EQ(table.intern("overflow"), bb::StringTable::kInvalidId);
}

BB_TEST(record_codec_roundtrip_inline) {
  const bb::Record original = rich_record();
  const std::vector<std::byte> bytes = encode(original, nullptr);
  bb::Record decoded{};
  CHECK(bb::decode_record_from(bytes, decoded, nullptr));
  check_record_equal(original, decoded);
}

BB_TEST(record_codec_roundtrip_with_string_table) {
  const bb::Record original = rich_record();
  bb::StringTable writer;
  const std::vector<std::byte> bytes = encode(original, &writer);
  CHECK(writer.size() > 0);
  bb::Record decoded{};
  CHECK(bb::decode_record_from(bytes, decoded, &writer));
  check_record_equal(original, decoded);
}

BB_TEST(record_codec_roundtrips_state_values) {
  const bb::Record original = state_record();
  bb::StringTable table;
  const std::vector<std::byte> bytes = encode(original, &table);
  bb::Record decoded{};
  CHECK(bb::decode_record_from(bytes, decoded, &table));
  check_record_equal(original, decoded);
  CHECK_EQ(bb::owned_value(decoded.state_old).as_string(), std::string("closed"));
  CHECK_EQ(bb::owned_value(decoded.state_new).as_string(), std::string("open"));
}

BB_TEST(record_codec_rejects_invalid_records) {
  std::array<std::byte, bb::kMaxEncodedRecordBytes> buffer{};
  bb::Encoder encoder(buffer);
  bb::Record invalid{};
  invalid.kind = static_cast<bb::RecordKind>(77);
  CHECK(!bb::encode_record(invalid, encoder, nullptr));

  bb::Record too_large{};
  too_large.attr_count = bb::kMaxAttributesPerRecord + 1;
  bb::Encoder second(buffer);
  CHECK(!bb::encode_record(too_large, second, nullptr));
}

BB_TEST(record_codec_fails_on_short_buffer) {
  const bb::Record original = rich_record();
  std::array<std::byte, 8> tiny{};
  bb::Encoder encoder(tiny);
  CHECK(!bb::encode_record(original, encoder, nullptr));
  CHECK(!encoder.ok());
}

BB_TEST(record_codec_rejects_every_truncation) {
  const bb::Record original = rich_record();
  const std::vector<std::byte> bytes = encode(original, nullptr);
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    bb::Record decoded{};
    const std::span<const std::byte> prefix(bytes.data(), length);
    CHECK(!bb::decode_record_from(prefix, decoded, nullptr));
  }
  bb::Record decoded{};
  CHECK(bb::decode_record_from(bytes, decoded, nullptr));
}

BB_TEST(record_codec_rejects_mismatched_string_table) {
  const bb::Record original = rich_record();
  bb::StringTable writer;
  const std::vector<std::byte> bytes = encode(original, &writer);
  bb::StringTable empty;
  bb::Record decoded{};
  CHECK(!bb::decode_record_from(bytes, decoded, &empty));
}

BB_TEST(record_codec_rejects_bad_version_and_kind) {
  const std::vector<std::byte> bytes = encode(rich_record(), nullptr);
  std::vector<std::byte> bad_version = bytes;
  bad_version[0] = std::byte{99};
  bb::Record decoded{};
  CHECK(!bb::decode_record_from(bad_version, decoded, nullptr));

  std::vector<std::byte> bad_kind = bytes;
  bad_kind[1] = std::byte{0};
  CHECK(!bb::decode_record_from(bad_kind, decoded, nullptr));
}

BB_TEST(record_codec_is_deterministic) {
  const bb::Record original = rich_record();
  const std::vector<std::byte> first = encode(original, nullptr);
  const std::vector<std::byte> second = encode(original, nullptr);
  CHECK(first == second);
}

BB_TEST(record_codec_survives_single_byte_corruption) {
  const bb::Record original = rich_record();
  const std::vector<std::byte> bytes = encode(original, nullptr);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    std::vector<std::byte> mutated = bytes;
    mutated[i] = std::byte{0xFF};
    bb::Record decoded{};
    if (bb::decode_record_from(mutated, decoded, nullptr)) {
      CHECK(bb::validate_record(decoded));
    }
  }
}

BB_TEST(record_codec_golden_bytes) {
  bb::Record record{};
  const std::vector<std::byte> bytes = encode(record, nullptr);
  const std::string hex = bb::to_hex(bytes.data(), bytes.size());
  CHECK_EQ(hex, std::string("0101020000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
}
