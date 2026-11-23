#include <blackbox/codec.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/query.hpp>
#include <blackbox/record.hpp>
#include <blackbox/string_table.hpp>
#include <blackbox/util/random.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::uint64_t g_failures = 0;

void failure(const char* target, const std::string& message) {
  ++g_failures;
  std::printf("FUZZ FAILURE [%s] %s\n", target, message.c_str());
}

bb::Record sample_record(std::uint64_t seq) {
  bb::Record record{};
  record.seq = seq;
  record.trace = bb::TraceId{0xF00D0000ull + seq, seq * 17 + 1};
  record.span = seq;
  record.correlation = seq % 3 == 0 ? 0 : seq;
  record.cause_seq = seq > 1 ? seq - 1 : 0;
  record.wall_ns = 1'700'000'000'000'000'000ll + static_cast<std::int64_t>(seq);
  record.mono_ns = static_cast<std::int64_t>(seq * 1000);
  record.attr_count = 2;
  bb::store_key("fuzz.index", record.attrs[0]);
  (void)bb::store_value(bb::Value::integer(static_cast<std::int64_t>(seq)),
                        record.attrs[0].value);
  bb::store_key("fuzz.text", record.attrs[1]);
  (void)bb::store_value(bb::Value::string("value-" + std::to_string(seq % 5)),
                        record.attrs[1].value);
  switch (seq % 7) {
    case 0:
      record.kind = bb::RecordKind::Log;
      record.level = bb::Level::Info;
      (void)bb::set_record_message(record, "fuzz log");
      break;
    case 1:
      record.kind = bb::RecordKind::Error;
      record.level = bb::Level::Error;
      record.status = bb::Status::Timeout;
      (void)bb::set_record_message(record, "fuzz timeout");
      break;
    case 2:
      record.kind = bb::RecordKind::Metric;
      record.level = bb::Level::Info;
      (void)bb::set_record_name(record, "fuzz.metric");
      record.int_value = static_cast<std::int64_t>(seq);
      (void)bb::store_value(bb::Value::integer(static_cast<std::int64_t>(seq)),
                            record.value);
      break;
    case 3:
      record.kind = bb::RecordKind::StateChange;
      record.level = bb::Level::Warn;
      (void)bb::set_record_state_key(record, "fuzz.state");
      (void)bb::store_value(bb::Value::string("closed"), record.state_old);
      (void)bb::store_value(bb::Value::string("open"), record.state_new);
      break;
    case 4:
      record.kind = bb::RecordKind::SpanBegin;
      record.level = bb::Level::Info;
      (void)bb::set_record_name(record, "fuzz.span");
      break;
    default:
      record.kind = bb::RecordKind::SpanEnd;
      record.level = bb::Level::Info;
      record.status = bb::Status::Ok;
      (void)bb::set_record_name(record, "fuzz.span");
      break;
  }
  return record;
}

bool check_dump_result(const char* target, const bb::DumpReadResult& result,
                       std::size_t input_size) {
  if (result.consumed_bytes > input_size) {
    failure(target, "consumed more bytes than provided");
    return false;
  }
  std::uint64_t previous = 0;
  for (const auto& record : result.records) {
    if (!bb::validate_record(record)) {
      failure(target, "invalid record survived the reader");
      return false;
    }
    if (record.seq == 0 || record.seq <= previous) {
      failure(target, "unordered sequence survived the reader");
      return false;
    }
    previous = record.seq;
  }
  if (result.complete && !result.ok) {
    failure(target, "complete dump marked not ok");
    return false;
  }
  if (!result.ok && result.error.empty()) {
    failure(target, "failure without an explanation");
    return false;
  }
  if (result.records.size() > 1'000'000) {
    failure(target, "record limit exceeded");
    return false;
  }
  return true;
}

void fuzz_dump(bb::Xoshiro128& random, int iterations,
               std::span<const std::byte> baseline) {
  const char* target = "dump";
  for (int iteration = 0; iteration < iterations; ++iteration) {
    std::vector<std::byte> input;
    const bool use_baseline = (random.next_u32() % 4) != 0;
    if (use_baseline) {
      input.assign(baseline.begin(), baseline.end());
      const unsigned operations = 1 + random.bounded(8);
      for (unsigned operation = 0; operation < operations; ++operation) {
        if (input.empty()) {
          break;
        }
        const std::size_t position = random.bounded(
            static_cast<std::uint32_t>(input.size()));
        input[position] = static_cast<std::byte>(
            std::to_integer<unsigned>(input[position]) ^
            (1u << random.bounded(8)));
      }
      if (random.next_u32() % 3 == 0 && !input.empty()) {
        input.resize(random.bounded(static_cast<std::uint32_t>(
            input.size())));
      }
    } else {
      input.resize(random.bounded(2048));
      for (auto& byte : input) {
        byte = static_cast<std::byte>(random.next_u32() & 0xFF);
      }
    }
    if (input.size() > 256u * 1024u * 1024u) {
      failure(target, "input grew beyond the limit");
      return;
    }
    bb::DumpReadOptions strict;
    const auto strict_result = bb::read_dump_bytes(input, strict);
    if (!check_dump_result(target, strict_result, input.size())) {
      return;
    }
    if (strict_result.ok && !strict_result.complete) {
      failure(target, "strict reader accepted an incomplete dump");
      return;
    }
    bb::DumpReadOptions recovery;
    recovery.require_footer = false;
    const auto recovery_result = bb::read_dump_bytes(input, recovery);
    if (!check_dump_result(target, recovery_result, input.size())) {
      return;
    }
  }
}

void fuzz_query(bb::Xoshiro128& random, int iterations,
                const std::vector<bb::Record>& records) {
  const char* target = "query";
  const std::string alphabet =
      "abcXYZ019_.-/ :()[]=!<>~\"'\\\n\tAND OR NOT kind level status "
      "attr seq duration state new true false 100ms error warn";
  const std::vector<std::string> bases = {
      "kind = error",
      "level >= warn AND message ~ timeout",
      "attr.fuzz.index >= 10 OR duration > 1ms",
      "trace ~ abcdef",
      "state.new = open",
      "NOT (kind = metric OR kind = log)",
      "attr[\"fuzz.text\"] = value-1",
  };
  for (int iteration = 0; iteration < iterations; ++iteration) {
    std::string text;
    if (random.next_u32() % 2 == 0) {
      const std::string& base = bases[random.bounded(
          static_cast<std::uint32_t>(bases.size()))];
      text = base;
      const unsigned mutations = 1 + random.bounded(4);
      for (unsigned mutation = 0; mutation < mutations; ++mutation) {
        if (text.empty()) {
          break;
        }
        const std::size_t position = random.bounded(
            static_cast<std::uint32_t>(text.size()));
        switch (random.bounded(3)) {
          case 0:
            text.erase(position, 1);
            break;
          case 1:
            text.insert(position, 1,
                        alphabet[random.bounded(
                            static_cast<std::uint32_t>(alphabet.size()))]);
            break;
          default:
            text[position] = alphabet[random.bounded(
                static_cast<std::uint32_t>(alphabet.size()))];
            break;
        }
      }
    } else {
      const std::uint32_t length = random.bounded(256);
      text.reserve(length);
      for (std::uint32_t i = 0; i < length; ++i) {
        text.push_back(alphabet[random.bounded(
            static_cast<std::uint32_t>(alphabet.size()))]);
      }
    }
    std::string error;
    const auto query = bb::Query::parse(text, &error);
    if (!query.has_value()) {
      if (error.empty()) {
        failure(target, "parse failure without an error message");
        return;
      }
      continue;
    }
    for (const auto& record : records) {
      (void)query->matches(record);
    }
  }
}

void fuzz_codec(bb::Xoshiro128& random, int iterations) {
  const char* target = "codec";
  const bb::Record record = sample_record(42);
  std::vector<std::byte> encoded(bb::kMaxEncodedRecordBytes);
  std::size_t written = 0;
  if (!bb::encode_record_to(record, encoded, nullptr, written)) {
    failure(target, "could not encode a valid record");
    return;
  }
  encoded.resize(written);
  for (int iteration = 0; iteration < iterations; ++iteration) {
    std::vector<std::byte> input = encoded;
    const unsigned operations = 1 + random.bounded(5);
    for (unsigned operation = 0; operation < operations; ++operation) {
      if (input.empty()) {
        break;
      }
      const std::size_t position = random.bounded(
          static_cast<std::uint32_t>(input.size()));
      input[position] = static_cast<std::byte>(random.next_u32() & 0xFF);
    }
    if (random.next_u32() % 4 == 0 && !input.empty()) {
      input.resize(random.bounded(static_cast<std::uint32_t>(input.size())));
    }
    bb::Record decoded{};
    if (bb::decode_record_from(input, decoded, nullptr)) {
      if (!bb::validate_record(decoded)) {
        failure(target, "decoder returned an invalid record");
        return;
      }
    }
  }
}

void fuzz_varint(bb::Xoshiro128& random, int iterations) {
  const char* target = "varint";
  for (int iteration = 0; iteration < iterations; ++iteration) {
    const std::uint64_t value = random.next_u64();
    std::array<std::byte, 16> buffer{};
    bb::Encoder encoder(buffer);
    encoder.varint(value);
    if (!encoder.ok()) {
      failure(target, "encoder overflowed a 16-byte buffer");
      return;
    }
    bb::Decoder decoder(encoder.written());
    if (decoder.varint() != value || !decoder.ok() || !decoder.at_end()) {
      failure(target, "varint round-trip failed");
      return;
    }
    std::vector<std::byte> garbage(random.bounded(32));
    for (auto& byte : garbage) {
      byte = static_cast<std::byte>(random.next_u32() & 0xFF);
    }
    bb::Decoder hostile(garbage);
    while (hostile.ok() && !hostile.at_end()) {
      (void)hostile.varint();
    }
    if (hostile.position() > garbage.size()) {
      failure(target, "decoder ran past the input");
      return;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 5000;
  std::uint64_t seed = 0xB1AC4B0Cull;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
      const long value = std::strtol(argv[++i], nullptr, 10);
      if (value <= 0 || value > 10'000'000) {
        std::printf("invalid iteration count\n");
        return 2;
      }
      iterations = static_cast<int>(value);
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else {
      std::printf("usage: blackbox_fuzz [--iterations N] [--seed N]\n");
      return 2;
    }
  }
  bb::Xoshiro128 random(seed);

  std::error_code error;
  const auto directory =
      std::filesystem::temp_directory_path(error) /
      ("blackbox-fuzz-" + bb::random_token(6));
  if (error) {
    std::printf("cannot create fuzz temp path\n");
    return 1;
  }
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::printf("cannot create fuzz temp directory\n");
    return 1;
  }
  const std::string path = (directory / "baseline.bbx").string();
  std::vector<bb::Record> records;
  for (std::uint64_t i = 1; i <= 64; ++i) {
    records.push_back(sample_record(i));
  }
  std::string dump_error;
  if (!bb::write_dump_file(path, records, {}, &dump_error)) {
    std::printf("cannot write fuzz baseline: %s\n", dump_error.c_str());
    std::filesystem::remove_all(directory, error);
    return 1;
  }
  const auto baseline = [&] {
    std::vector<std::byte> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      return bytes;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
      bytes.resize(static_cast<std::size_t>(size));
      const std::size_t read =
          std::fread(bytes.data(), 1, bytes.size(), file);
      bytes.resize(read);
    }
    std::fclose(file);
    return bytes;
  }();
  std::printf("blackbox fuzz: %d iterations, seed %llu, baseline %zu bytes\n",
              iterations, static_cast<unsigned long long>(seed), baseline.size());

  fuzz_varint(random, iterations);
  fuzz_codec(random, iterations);
  fuzz_query(random, iterations, records);
  fuzz_dump(random, iterations, baseline);

  std::filesystem::remove_all(directory, error);
  if (g_failures != 0) {
    std::printf("FAILED: %llu invariant violation(s)\n",
                static_cast<unsigned long long>(g_failures));
    return 1;
  }
  std::printf("OK: no invariant violations\n");
  return 0;
}
