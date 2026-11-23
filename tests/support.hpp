#pragma once

#include <blackbox/record.hpp>
#include <blackbox/util/random.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bbtest {

/// Unique temporary directory, removed recursively on destruction.
class TempDir {
 public:
  TempDir() {
    const std::string suffix = bb::random_token(8);
    path_ = std::filesystem::temp_directory_path() /
            ("blackbox-test-" + suffix);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string file(const std::string& name) const {
    return (path_ / name).string();
  }

 private:
  std::filesystem::path path_;
};

inline bb::Record sample_record(std::uint64_t seq) {
  bb::Record record{};
  record.seq = seq;
  record.trace = bb::TraceId{0x1000000000000000ull + seq, seq * 3 + 1};
  record.span = seq;
  record.correlation = seq;
  record.cause_seq = seq > 1 ? seq - 1 : 0;
  record.wall_ns = 1'700'000'000'000'000'000ll + static_cast<std::int64_t>(seq);
  record.mono_ns = static_cast<std::int64_t>(seq * 100);
  record.duration_ns = static_cast<std::int64_t>(seq * 1000);
  record.attr_count = 2;
  bb::store_key("index", record.attrs[0]);
  const bool stored_index =
      bb::store_value(bb::Value::integer(static_cast<std::int64_t>(seq)),
                      record.attrs[0].value);
  bb::store_key("tag", record.attrs[1]);
  const bool stored_tag =
      bb::store_value(bb::Value::string("t" + std::to_string(seq % 3)),
                      record.attrs[1].value);
  if (!stored_index || !stored_tag) {
    return bb::Record{};
  }
  switch (seq % 6) {
    case 0:
      record.kind = bb::RecordKind::Log;
      record.level = bb::Level::Info;
      bb::set_record_message(record, "log message " + std::to_string(seq));
      break;
    case 1:
      record.kind = bb::RecordKind::Error;
      record.level = bb::Level::Error;
      record.status = bb::Status::Timeout;
      bb::set_record_message(record, "timeout " + std::to_string(seq));
      break;
    case 2:
      record.kind = bb::RecordKind::Metric;
      record.level = bb::Level::Info;
      bb::set_record_name(record, "metric." + std::to_string(seq % 4));
      record.int_value = static_cast<std::int64_t>(seq);
      (void)bb::store_value(bb::Value::integer(static_cast<std::int64_t>(seq)),
                            record.value);
      break;
    case 3:
      record.kind = bb::RecordKind::StateChange;
      record.level = bb::Level::Warn;
      bb::set_record_state_key(record, "state.key");
      (void)bb::store_value(bb::Value::string("old"), record.state_old);
      (void)bb::store_value(bb::Value::string("new"), record.state_new);
      bb::set_record_message(record, "state change");
      break;
    case 4:
      record.kind = bb::RecordKind::SpanBegin;
      record.level = bb::Level::Info;
      bb::set_record_name(record, "span." + std::to_string(seq % 2));
      record.parent = seq > 4 ? seq - 4 : 0;
      break;
    default:
      record.kind = bb::RecordKind::SpanEnd;
      record.level = bb::Level::Info;
      record.status = bb::Status::Ok;
      bb::set_record_name(record, "span." + std::to_string(seq % 2));
      break;
  }
  return record;
}

inline std::vector<bb::Record> sample_records(std::size_t count,
                                              std::uint64_t first_seq = 1) {
  std::vector<bb::Record> records;
  records.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    records.push_back(sample_record(first_seq + i));
  }
  return records;
}

inline bool write_file_bytes(const std::string& path,
                             const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  return static_cast<bool>(out);
}

inline std::vector<std::byte> read_file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return {};
  }
  in.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  }
  return bytes;
}

}  // namespace bbtest
