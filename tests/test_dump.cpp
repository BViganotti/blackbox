#include "harness.hpp"
#include "support.hpp"

#include <blackbox/dump.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/hex.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void check_records_match(const std::vector<bb::Record>& actual,
                         const std::vector<bb::Record>& expected) {
  CHECK_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
    CHECK_EQ(actual[i].seq, expected[i].seq);
    CHECK_EQ(static_cast<int>(actual[i].kind),
             static_cast<int>(expected[i].kind));
    CHECK_EQ(static_cast<int>(actual[i].level),
             static_cast<int>(expected[i].level));
    CHECK_EQ(static_cast<int>(actual[i].status),
             static_cast<int>(expected[i].status));
    CHECK(actual[i].trace == expected[i].trace);
    CHECK_EQ(actual[i].span, expected[i].span);
    CHECK_EQ(actual[i].parent, expected[i].parent);
    CHECK_EQ(actual[i].correlation, expected[i].correlation);
    CHECK_EQ(actual[i].cause_seq, expected[i].cause_seq);
    CHECK_EQ(actual[i].duration_ns, expected[i].duration_ns);
    CHECK_EQ(bb::record_name(actual[i]), bb::record_name(expected[i]));
    CHECK_EQ(bb::record_message(actual[i]), bb::record_message(expected[i]));
    CHECK_EQ(bb::record_state_key(actual[i]),
             bb::record_state_key(expected[i]));
    CHECK(bb::owned_value(actual[i].state_old) ==
          bb::owned_value(expected[i].state_old));
    CHECK(bb::owned_value(actual[i].state_new) ==
          bb::owned_value(expected[i].state_new));
    CHECK_EQ(static_cast<int>(actual[i].attr_count),
             static_cast<int>(expected[i].attr_count));
  }
}

}  // namespace

BB_TEST(dump_roundtrip_preserves_every_field) {
  bbtest::TempDir temp;
  const std::vector<bb::Record> records = bbtest::sample_records(12);
  const std::string path = temp.file("trace.bbx");
  bb::DumpWriteOptions options;
  options.created_wall_ns = 1'700'000'000'123'456'789ll;
  std::string error;
  CHECK(bb::write_dump_file(path, records, options, &error));
  CHECK(error.empty());
  CHECK(std::filesystem::exists(path));
  const auto bytes = bbtest::read_file_bytes(path);
  CHECK(bytes.size() > 100);
  const auto result = bb::read_dump_file(path);
  CHECK(result.ok);
  CHECK(result.complete);
  CHECK(!result.truncated);
  CHECK(!result.corrupted);
  CHECK_EQ(result.records.size(), records.size());
  CHECK_EQ(result.meta.record_count, records.size());
  CHECK_EQ(result.meta.first_seq, records.front().seq);
  CHECK_EQ(result.meta.last_seq, records.back().seq);
  CHECK_EQ(result.meta.created_wall_ns, options.created_wall_ns);
  CHECK(result.meta.string_count > 0);
  check_records_match(result.records, records);
}

BB_TEST(dump_roundtrip_inline_strings) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(6);
  const std::string path = temp.file("inline.bbx");
  bb::DumpWriteOptions options;
  options.inline_strings = true;
  options.extra_flags = bb::kDumpFlagCrash;
  std::string error;
  CHECK(bb::write_dump_file(path, records, options, &error));
  const auto result = bb::read_dump_file(path);
  CHECK(result.ok);
  CHECK(result.complete);
  CHECK((result.meta.flags & bb::kDumpFlagInlineStrings) != 0);
  CHECK((result.meta.flags & bb::kDumpFlagCrash) != 0);
  check_records_match(result.records, records);
}

BB_TEST(dump_empty_is_valid) {
  bbtest::TempDir temp;
  const std::string path = temp.file("empty.bbx");
  CHECK(bb::write_dump_file(path, {}, {}, nullptr));
  const auto result = bb::read_dump_file(path);
  CHECK(result.ok);
  CHECK(result.complete);
  CHECK(result.records.empty());
  CHECK_EQ(result.meta.record_count, 0u);
  CHECK_EQ(result.meta.first_seq, 0u);
}

BB_TEST(dump_writer_rejects_invalid_input) {
  bbtest::TempDir temp;
  std::vector<bb::Record> records;
  bb::Record invalid{};
  invalid.kind = static_cast<bb::RecordKind>(99);
  records.push_back(invalid);
  std::string error;
  CHECK(!bb::write_dump_file(temp.file("bad.bbx"), records, {}, &error));
  CHECK(!error.empty());
  CHECK(!std::filesystem::exists(temp.file("bad.bbx")));

  auto unordered = bbtest::sample_records(3);
  std::swap(unordered[0], unordered[2]);
  error.clear();
  CHECK(!bb::write_dump_file(temp.file("unordered.bbx"), unordered, {},
                             &error));
  CHECK(!std::filesystem::exists(temp.file("unordered.bbx")));
}

BB_TEST(dump_metadata_matches_full_parse) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(4);
  const std::string path = temp.file("meta.bbx");
  CHECK(bb::write_dump_file(path, records, {}, nullptr));
  bb::DumpMetadata metadata;
  std::string error;
  CHECK(bb::read_dump_metadata(path, metadata, &error));
  CHECK(error.empty());
  CHECK_EQ(metadata.record_count, records.size());
  CHECK_EQ(metadata.first_seq, records.front().seq);
  CHECK_EQ(metadata.last_seq, records.back().seq);
  CHECK(metadata.file_size > 0);
}

BB_TEST(dump_truncation_fails_strict_and_recovers) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(5);
  const std::string path = temp.file("full.bbx");
  CHECK(bb::write_dump_file(path, records, {}, nullptr));
  const std::vector<std::byte> bytes = bbtest::read_file_bytes(path);
  CHECK(bytes.size() > 64);

  bool saw_partial = false;
  for (std::size_t length = 0; length < bytes.size(); length += 3) {
    const std::span<const std::byte> prefix(bytes.data(), length);
    bb::DumpReadOptions strict;
    const auto strict_result = bb::read_dump_bytes(prefix, strict);
    CHECK(!strict_result.ok);

    bb::DumpReadOptions recovery;
    recovery.require_footer = false;
    const auto recovered = bb::read_dump_bytes(prefix, recovery);
    if (length < 64) {
      CHECK(!recovered.ok);
    } else {
      CHECK(recovered.ok);
      CHECK(!recovered.complete);
      CHECK(recovered.truncated);
      for (const auto& record : recovered.records) {
        CHECK(bb::validate_record(record));
      }
      if (!recovered.records.empty() &&
          recovered.records.size() < records.size()) {
        saw_partial = true;
      }
    }
  }
  CHECK(saw_partial);
}

BB_TEST(dump_corrupt_payload_is_rejected) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(8);
  const std::string path = temp.file("corrupt.bbx");
  CHECK(bb::write_dump_file(path, records, {}, nullptr));
  std::vector<std::byte> bytes = bbtest::read_file_bytes(path);

  // Corrupt a byte well inside the first record frame payload.
  const std::size_t position = bytes.size() / 2;
  bytes[position] = static_cast<std::byte>(
      std::to_integer<unsigned>(bytes[position]) ^ 0x5A);

  bb::DumpReadOptions strict;
  const auto strict_result = bb::read_dump_bytes(bytes, strict);
  CHECK(!strict_result.ok);
  CHECK(!strict_result.error.empty());

  bb::DumpReadOptions recovery;
  recovery.require_footer = false;
  const auto recovered = bb::read_dump_bytes(bytes, recovery);
  CHECK(recovered.ok);
  CHECK(recovered.corrupted);
  CHECK(!recovered.complete);
  for (const auto& record : recovered.records) {
    CHECK(bb::validate_record(record));
  }
}

BB_TEST(dump_bad_header_is_rejected_without_data) {
  std::vector<std::byte> garbage(128);
  for (std::size_t i = 0; i < garbage.size(); ++i) {
    garbage[i] = static_cast<std::byte>(i * 37);
  }
  const auto result = bb::read_dump_bytes(garbage);
  CHECK(!result.ok);
  CHECK(result.records.empty());
  CHECK(!result.error.empty());

  const std::array<std::byte, 10> tiny{};
  const auto short_result = bb::read_dump_bytes(tiny);
  CHECK(!short_result.ok);
  CHECK(short_result.records.empty());
}

BB_TEST(dump_checksum_corruption_in_footer_is_detected) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(3);
  const std::string path = temp.file("footer.bbx");
  CHECK(bb::write_dump_file(path, records, {}, nullptr));
  std::vector<std::byte> bytes = bbtest::read_file_bytes(path);
  bytes.back() = static_cast<std::byte>(0xFF);
  bb::DumpReadOptions recovery;
  recovery.require_footer = false;
  const auto result = bb::read_dump_bytes(bytes, recovery);
  CHECK(result.ok);
  CHECK(result.corrupted);
  CHECK(result.records.size() <= records.size());
  for (const auto& record : result.records) {
    CHECK(bb::validate_record(record));
  }
}

BB_TEST(dump_random_bytes_never_crash) {
  bb::Xoshiro128 random(0xC0FFEE);
  for (int iteration = 0; iteration < 500; ++iteration) {
    std::vector<std::byte> bytes(random.bounded(1024));
    for (auto& byte : bytes) {
      byte = static_cast<std::byte>(random.next_u32() & 0xFF);
    }
    const auto strict = bb::read_dump_bytes(bytes);
    CHECK(!strict.ok || strict.records.empty());
    bb::DumpReadOptions recovery;
    recovery.require_footer = false;
    const auto recovered = bb::read_dump_bytes(bytes, recovery);
    for (const auto& record : recovered.records) {
      CHECK(bb::validate_record(record));
    }
  }
}

BB_TEST(dump_listing_finds_and_orders_files) {
  bbtest::TempDir temp;
  const auto first = bbtest::sample_records(3);
  const auto second = bbtest::sample_records(2, 100);
  bb::DumpWriteOptions older;
  older.created_wall_ns = 1'000;
  bb::DumpWriteOptions newer;
  newer.created_wall_ns = 2'000;
  CHECK(bb::write_dump_file(temp.file("first.bbx"), first, older, nullptr));
  CHECK(bb::write_dump_file(temp.file("second.bbx"), second, newer, nullptr));
  CHECK(bb::write_dump_file(temp.file("ignored.txt"), first, older, nullptr));
  bbtest::write_file_bytes(temp.file("broken.bbx"),
                           std::vector<std::byte>(10, std::byte{0}));

  const auto dumps = bb::list_dump_files(temp.path().string());
  CHECK_EQ(dumps.size(), 3u);
  CHECK_EQ(dumps[0].name, std::string("second.bbx"));
  CHECK(dumps[0].valid);
  CHECK_EQ(dumps[0].meta.created_wall_ns, newer.created_wall_ns);
  CHECK_EQ(dumps[1].name, std::string("first.bbx"));
  CHECK(dumps[1].valid);
  CHECK(!dumps[2].valid);
  CHECK_EQ(dumps[2].name, std::string("broken.bbx"));
  const auto limited = bb::list_dump_files(temp.path().string(), 1);
  CHECK_EQ(limited.size(), 1u);
}

namespace {

bb::Record random_record(bb::Xoshiro128& random, std::uint64_t seq) {
  bb::Record record{};
  record.seq = seq;
  record.trace = bb::TraceId{seq * 0x9E3779B1u + 1, seq * 0x85EBCA77u + 2};
  record.span = seq;
  record.correlation = seq % 4 == 0 ? 0 : seq;
  record.cause_seq = seq > 1 ? seq - 1 : 0;
  record.wall_ns = 1'700'000'000'000'000'000ll + static_cast<std::int64_t>(seq);
  record.mono_ns = static_cast<std::int64_t>(seq * 13);
  record.duration_ns = static_cast<std::int64_t>(random.bounded(1'000'000));
  record.attr_count = static_cast<std::uint8_t>(random.bounded(4));
  for (std::uint8_t i = 0; i < record.attr_count; ++i) {
    const std::string key = "attr" + std::to_string(i);
    bb::store_key(key, record.attrs[i]);
    switch (random.bounded(5)) {
      case 0:
        (void)bb::store_value(bb::Value::null(), record.attrs[i].value);
        break;
      case 1:
        (void)bb::store_value(bb::Value::boolean(random.next_u32() % 2 == 0),
                              record.attrs[i].value);
        break;
      case 2:
        (void)bb::store_value(
            bb::Value::integer(static_cast<std::int64_t>(random.next_u32())),
            record.attrs[i].value);
        break;
      case 3:
        (void)bb::store_value(
            bb::Value::floating(static_cast<double>(random.next_u32()) / 7.0),
            record.attrs[i].value);
        break;
      default:
        (void)bb::store_value(
            bb::Value::string("text-" + std::to_string(random.bounded(200))),
            record.attrs[i].value);
        break;
    }
  }
  switch (random.bounded(8)) {
    case 0:
      record.kind = bb::RecordKind::Log;
      record.level = bb::Level::Info;
      (void)bb::set_record_message(record, "random log");
      break;
    case 1:
      record.kind = bb::RecordKind::Error;
      record.level = bb::Level::Error;
      record.status = random.next_u32() % 2 == 0 ? bb::Status::Timeout
                                                 : bb::Status::Error;
      (void)bb::set_record_message(record, "random error");
      break;
    case 2:
      record.kind = bb::RecordKind::Metric;
      record.level = bb::Level::Info;
      (void)bb::set_record_name(record, "random.metric");
      record.int_value = static_cast<std::int64_t>(random.next_u32());
      (void)bb::store_value(bb::Value::integer(record.int_value), record.value);
      break;
    case 3:
      record.kind = bb::RecordKind::StateChange;
      record.level = bb::Level::Warn;
      (void)bb::set_record_state_key(record, "random.state");
      (void)bb::store_value(
          bb::Value::integer(static_cast<std::int64_t>(random.bounded(10))),
          record.state_old);
      (void)bb::store_value(
          bb::Value::integer(static_cast<std::int64_t>(random.bounded(10))),
          record.state_new);
      break;
    case 4:
      record.kind = bb::RecordKind::SpanBegin;
      record.level = bb::Level::Info;
      (void)bb::set_record_name(record, "random.span");
      break;
    case 5:
      record.kind = bb::RecordKind::SpanEnd;
      record.level = bb::Level::Info;
      record.status = bb::Status::Ok;
      (void)bb::set_record_name(record, "random.span");
      break;
    case 6:
      record.kind = bb::RecordKind::Mark;
      record.level = bb::Level::Info;
      (void)bb::set_record_name(record, "random.mark");
      break;
    default:
      record.kind = bb::RecordKind::Note;
      record.level = bb::Level::Debug;
      (void)bb::set_record_message(record, "random note");
      break;
  }
  return record;
}

}  // namespace

BB_TEST(dump_randomized_roundtrip) {
  bbtest::TempDir temp;
  bb::Xoshiro128 random(0xD00DFEED);
  for (int iteration = 0; iteration < 40; ++iteration) {
    const std::size_t count = 1 + random.bounded(24);
    std::vector<bb::Record> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      records.push_back(random_record(random, i + 1));
      CHECK(bb::validate_record(records.back()));
    }
    const std::string path = temp.file("random-" + std::to_string(iteration) + ".bbx");
    std::string error;
    CHECK(bb::write_dump_file(path, records, {}, &error));
    CHECK(error.empty());
    const auto read = bb::read_dump_file(path);
    CHECK(read.ok);
    CHECK(read.complete);
    CHECK_EQ(read.records.size(), records.size());
    check_records_match(read.records, records);
  }
}

BB_TEST(dump_writer_enforces_byte_limit) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(20);
  const std::string path = temp.file("too-big.bbx");
  bb::DumpWriteOptions options;
  options.max_file_bytes = 1024;
  std::string error;
  CHECK(!bb::write_dump_file(path, records, options, &error));
  CHECK(!error.empty());
  CHECK(!std::filesystem::exists(path));
  for (const auto& entry : std::filesystem::directory_iterator(temp.path())) {
    CHECK(entry.path().extension() != ".tmp");
  }
  error.clear();
  options.max_file_bytes = 32;
  CHECK(!bb::write_dump_file(path, records, options, &error));
}

BB_TEST(dump_metadata_reports_footer_integrity) {
  bbtest::TempDir temp;
  const auto records = bbtest::sample_records(4);
  const std::string path = temp.file("footer-check.bbx");
  CHECK(bb::write_dump_file(path, records, {}, nullptr));
  bb::DumpMetadata metadata;
  std::string error;
  CHECK(bb::read_dump_metadata(path, metadata, &error));
  CHECK(metadata.footer_valid);
  CHECK_EQ(metadata.record_count, records.size());

  std::vector<std::byte> bytes = bbtest::read_file_bytes(path);
  CHECK(bytes.size() > 32);
  bytes.back() = static_cast<std::byte>(0xFF);
  CHECK(bbtest::write_file_bytes(path, bytes));

  metadata = bb::DumpMetadata{};
  CHECK(bb::read_dump_metadata(path, metadata, &error));
  CHECK(!metadata.footer_valid);

  const auto dumps = bb::list_dump_files(temp.path().string());
  CHECK_EQ(dumps.size(), 1u);
  CHECK(dumps[0].valid);
  CHECK(!dumps[0].complete);

  const auto strict = bb::read_dump_file(path);
  CHECK(!strict.ok);
  bb::DumpReadOptions recovery;
  recovery.require_footer = false;
  const auto recovered = bb::read_dump_file(path, recovery);
  CHECK(recovered.ok);
  CHECK(recovered.corrupted);
  CHECK_EQ(recovered.records.size(), records.size());
}
