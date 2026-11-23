#include "harness.hpp"
#include "support.hpp"

#include <blackbox/dump.hpp>
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
