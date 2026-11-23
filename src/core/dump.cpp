#include <blackbox/dump.hpp>

#include <blackbox/codec.hpp>
#include <blackbox/string_table.hpp>
#include <blackbox/util/crc32c.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>
#include <blackbox/util/varint.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bb {
namespace {

constexpr char kHeaderMagic[8] = {'B', 'B', 'X', 'D', 'M', 'P', '0', '1'};
constexpr char kFooterMagic[8] = {'B', 'B', 'X', 'E', 'N', 'D', '0', '1'};
constexpr std::uint32_t kDumpVersion = 1;
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kFooterSize = 32;
constexpr std::size_t kFrameHeaderSize = 9;
constexpr std::uint32_t kFooterEndMarker = 0x42425845u;
constexpr std::size_t kMaxWriteRecords = 1'000'000;
constexpr std::size_t kMaxPathBytes = 3500;

enum FrameType : std::uint8_t {
  kFrameStrings = 1,
  kFrameRecord = 2,
  kFrameMeta = 3,
};

struct ParsedHeader {
  std::uint32_t version = 0;
  std::uint32_t flags = 0;
  std::uint32_t header_size = 0;
  std::int64_t created_wall_ns = 0;
  std::uint64_t record_count = 0;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  std::uint32_t string_count = 0;
};

bool parse_header(std::span<const std::byte> bytes, ParsedHeader& out,
                  std::string& error) {
  if (bytes.size() < kHeaderSize) {
    error = "dump header truncated";
    return false;
  }
  if (std::memcmp(bytes.data(), kHeaderMagic, sizeof(kHeaderMagic)) != 0) {
    error = "not a blackbox dump (bad magic)";
    return false;
  }
  Decoder decoder(bytes.first(kHeaderSize - 4));
  char magic[8];
  if (!decoder.bytes(magic, sizeof(magic))) {
    error = "dump header truncated";
    return false;
  }
  out.version = decoder.u32();
  out.flags = decoder.u32();
  out.header_size = decoder.u32();
  out.created_wall_ns = static_cast<std::int64_t>(decoder.u64());
  out.record_count = decoder.u64();
  out.first_seq = decoder.u64();
  out.last_seq = decoder.u64();
  out.string_count = decoder.u32();
  const std::uint32_t reserved = decoder.u32();
  if (!decoder.ok()) {
    error = "dump header truncated";
    return false;
  }
  Decoder crc_decoder(bytes.subspan(kHeaderSize - 4, 4));
  const std::uint32_t stored_crc = crc_decoder.u32();
  if (!crc_decoder.ok() ||
      stored_crc != crc32c(bytes.data(), kHeaderSize - 4)) {
    error = "dump header checksum mismatch";
    return false;
  }
  if (out.version != kDumpVersion) {
    error = "unsupported dump version";
    return false;
  }
  if (out.header_size != kHeaderSize) {
    error = "unexpected dump header size";
    return false;
  }
  if (reserved != 0) {
    error = "dump header reserved field is not zero";
    return false;
  }
  if ((out.flags & ~static_cast<std::uint32_t>(kDumpKnownFlags)) != 0) {
    error = "unknown dump flags";
    return false;
  }
  return true;
}

void encode_header(std::span<std::byte> out, std::uint16_t flags,
                   std::int64_t created_wall_ns, std::uint64_t record_count,
                   std::uint64_t first_seq, std::uint64_t last_seq,
                   std::uint32_t string_count) {
  Encoder encoder(out);
  encoder.bytes(kHeaderMagic, sizeof(kHeaderMagic));
  encoder.u32(kDumpVersion);
  encoder.u32(flags);
  encoder.u32(static_cast<std::uint32_t>(kHeaderSize));
  encoder.u64(static_cast<std::uint64_t>(created_wall_ns));
  encoder.u64(record_count);
  encoder.u64(first_seq);
  encoder.u64(last_seq);
  encoder.u32(string_count);
  encoder.u32(0);
  encoder.u32(crc32c(out.data(), out.size() - 4));
}

bool parse_footer(std::span<const std::byte> bytes, std::uint64_t& count,
                  std::uint32_t& records_crc, std::string& error) {
  if (bytes.size() < kFooterSize) {
    error = "dump footer truncated";
    return false;
  }
  if (std::memcmp(bytes.data(), kFooterMagic, sizeof(kFooterMagic)) != 0) {
    error = "dump footer magic mismatch";
    return false;
  }
  Decoder decoder(bytes);
  char magic[8];
  decoder.bytes(magic, sizeof(magic));
  count = decoder.u64();
  records_crc = decoder.u32();
  const std::uint32_t reserved = decoder.u32();
  const std::uint32_t footer_crc = decoder.u32();
  const std::uint32_t end_marker = decoder.u32();
  if (!decoder.ok()) {
    error = "dump footer truncated";
    return false;
  }
  if (footer_crc != crc32c(bytes.data(), 24)) {
    error = "dump footer checksum mismatch";
    return false;
  }
  if (end_marker != kFooterEndMarker) {
    error = "dump footer end marker mismatch";
    return false;
  }
  if (reserved != 0) {
    error = "dump footer reserved field is not zero";
    return false;
  }
  return true;
}

class OutputFile {
 public:
  void set_limit(std::size_t limit) { limit_ = limit; }

  OutputFile() = default;
  ~OutputFile() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (!committed_ && !temp_path_.empty()) {
      ::unlink(temp_path_.c_str());
    }
  }
  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;

  bool open(std::string_view final_path, std::string& error) {
    if (final_path.empty() || final_path.size() > kMaxPathBytes) {
      error = "invalid dump path";
      return false;
    }
    final_path_.assign(final_path);
    std::string suffix;
    try {
      suffix = random_token(4);
    } catch (const std::exception&) {
      suffix = "00000000";
    }
    temp_path_ = final_path_ + ".tmp." + std::to_string(::getpid()) + "." + suffix;
    fd_ = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                 0600);
    if (fd_ < 0) {
      error = std::string("cannot create dump: ") + std::strerror(errno);
      temp_path_.clear();
      return false;
    }
    return true;
  }

  bool write_all(const void* data, std::size_t size, std::string& error) {
    if (size > limit_ || written_ > limit_ - size) {
      error = "dump exceeds the configured file size limit";
      return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t written =
          ::write(fd_, bytes + offset, size - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        error = std::string("dump write failed: ") + std::strerror(errno);
        return false;
      }
      if (written == 0) {
        error = "dump write returned zero";
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    written_ += size;
    return true;
  }

  bool commit(std::string& error) {
    if (::fsync(fd_) != 0) {
      error = std::string("dump fsync failed: ") + std::strerror(errno);
      return false;
    }
    if (::close(fd_) != 0) {
      fd_ = -1;
      error = std::string("dump close failed: ") + std::strerror(errno);
      return false;
    }
    fd_ = -1;
    if (::rename(temp_path_.c_str(), final_path_.c_str()) != 0) {
      error = std::string("dump rename failed: ") + std::strerror(errno);
      return false;
    }
    committed_ = true;
    return true;
  }

 private:
  int fd_ = -1;
  std::string final_path_;
  std::string temp_path_;
  std::size_t written_ = 0;
  std::size_t limit_ = kDefaultMaxDumpFileBytes;
  bool committed_ = false;
};

bool write_frame(OutputFile& file, std::uint8_t type,
                 std::span<const std::byte> payload, std::string& error) {
  std::array<std::byte, kFrameHeaderSize> header{};
  Encoder encoder(header);
  encoder.u8(type);
  encoder.u32(static_cast<std::uint32_t>(payload.size()));
  encoder.u32(crc32c(payload));
  if (!encoder.ok()) {
    error = "frame header too large";
    return false;
  }
  return file.write_all(header.data(), header.size(), error) &&
         file.write_all(payload.data(), payload.size(), error);
}

bool add_record_strings(const Record& record, StringTable& table) {
  const auto add = [&table](std::string_view text) {
    return table.intern(text) != StringTable::kInvalidId;
  };
  if (!add(record_name(record)) || !add(record_message(record)) ||
      !add(record_state_key(record))) {
    return false;
  }
  const auto add_value = [&add](const StoredValue& value) {
    return value.type != ValueType::String || add(stored_text(value));
  };
  if (!add_value(record.value) || !add_value(record.state_old) ||
      !add_value(record.state_new)) {
    return false;
  }
  for (std::size_t i = 0; i < record.attr_count; ++i) {
    if (!add(stored_key(record.attrs[i])) ||
        !add_value(record.attrs[i].value)) {
      return false;
    }
  }
  return true;
}

bool read_entire_file(std::string_view path, std::size_t max_bytes,
                      std::vector<std::byte>& out, std::uint64_t& file_size,
                      std::string& error) {
  const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = std::string("cannot open dump: ") + std::strerror(errno);
    return false;
  }
  struct stat status {};
  if (::fstat(fd, &status) != 0) {
    error = std::string("cannot stat dump: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    error = "dump path is not a regular file";
    ::close(fd);
    return false;
  }
  if (status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > max_bytes) {
    error = "dump file exceeds the size limit";
    ::close(fd);
    return false;
  }
  const auto size = static_cast<std::size_t>(status.st_size);
  out.assign(size, std::byte{0});
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t got = ::read(fd, out.data() + offset, size - offset);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::string("dump read failed: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    offset += static_cast<std::size_t>(got);
  }
  ::close(fd);
  out.resize(offset);
  file_size = static_cast<std::uint64_t>(offset);
  return true;
}

}  // namespace

bool write_dump_file(std::string_view path, std::span<const Record> records,
                     const DumpWriteOptions& options, std::string* error) {
  std::string local_error;
  std::string& err = error != nullptr ? *error : local_error;
  err.clear();
  if (records.size() > kMaxWriteRecords) {
    err = "too many records for one dump";
    return false;
  }
  if ((options.extra_flags & ~kDumpKnownFlags) != 0) {
    err = "unknown dump flags requested";
    return false;
  }
  if (options.max_file_bytes < kHeaderSize + kFooterSize) {
    err = "dump file size limit is smaller than the header and footer";
    return false;
  }
  std::uint64_t previous = 0;
  for (const Record& record : records) {
    if (!validate_record(record)) {
      err = "refusing to write an invalid record";
      return false;
    }
    if (record.seq == 0 || record.seq <= previous) {
      err = "refusing to write unordered records";
      return false;
    }
    previous = record.seq;
  }

  const std::uint16_t flags = static_cast<std::uint16_t>(
      (options.inline_strings ? kDumpFlagInlineStrings : 0) |
      options.extra_flags);
  StringTable table;
  if (!options.inline_strings) {
    for (const Record& record : records) {
      if (!add_record_strings(record, table)) {
        err = "dump string table exhausted";
        return false;
      }
    }
  }

  OutputFile file;
  file.set_limit(options.max_file_bytes);
  if (!file.open(path, err)) {
    return false;
  }

  std::array<std::byte, kHeaderSize> header{};
  encode_header(header, flags,
                options.created_wall_ns != 0 ? options.created_wall_ns
                                             : now_wall_ns(),
                static_cast<std::uint64_t>(records.size()),
                records.empty() ? 0 : records.front().seq,
                records.empty() ? 0 : records.back().seq,
                static_cast<std::uint32_t>(table.size()));
  if (!file.write_all(header.data(), header.size(), err)) {
    return false;
  }

  if (!options.inline_strings) {
    std::size_t payload_size = 10;
    for (const std::string& text : table.strings()) {
      payload_size += uvarint_size(text.size()) + text.size();
    }
    std::vector<std::byte> payload(payload_size);
    Encoder encoder(payload);
    encoder.varint(table.size());
    for (const std::string& text : table.strings()) {
      encoder.raw_string(text);
    }
    if (!encoder.ok() ||
        !write_frame(file, kFrameStrings, encoder.written(), err)) {
      if (err.empty()) {
        err = "failed to encode string table";
      }
      return false;
    }
  }

  Crc32c records_crc;
  std::array<std::byte, kMaxEncodedRecordBytes> encoded{};
  for (const Record& record : records) {
    Encoder encoder(encoded);
    if (!encode_record(record, encoder, options.inline_strings ? nullptr
                                                              : &table)) {
      err = "failed to encode record";
      return false;
    }
    records_crc.update(encoder.written());
    if (!write_frame(file, kFrameRecord, encoder.written(), err)) {
      return false;
    }
  }

  std::array<std::byte, kFooterSize> footer{};
  Encoder footer_encoder(footer);
  footer_encoder.bytes(kFooterMagic, sizeof(kFooterMagic));
  footer_encoder.u64(static_cast<std::uint64_t>(records.size()));
  footer_encoder.u32(records_crc.value());
  footer_encoder.u32(0);
  footer_encoder.u32(crc32c(footer.data(), 24));
  footer_encoder.u32(kFooterEndMarker);
  if (!footer_encoder.ok() ||
      !file.write_all(footer.data(), footer.size(), err)) {
    if (err.empty()) {
      err = "failed to encode dump footer";
    }
    return false;
  }
  return file.commit(err);
}

DumpReadResult read_dump_bytes(std::span<const std::byte> bytes,
                               const DumpReadOptions& options) {
  DumpReadResult result;
  ParsedHeader header;
  std::string error;
  if (!parse_header(bytes, header, error)) {
    result.error = error;
    return result;
  }
  result.meta.version = static_cast<std::uint16_t>(header.version);
  result.meta.flags = static_cast<std::uint16_t>(header.flags);
  result.meta.created_wall_ns = header.created_wall_ns;
  result.meta.record_count = header.record_count;
  result.meta.first_seq = header.first_seq;
  result.meta.last_seq = header.last_seq;
  result.meta.string_count = header.string_count;

  const bool inline_strings =
      (header.flags & kDumpFlagInlineStrings) != 0;
  StringTable table;
  bool strings_seen = false;
  Crc32c records_crc;
  std::uint64_t previous_seq = 0;
  std::size_t offset = kHeaderSize;
  bool footer_seen = false;
  bool stop = false;
  std::uint64_t footer_count = 0;
  std::uint32_t footer_records_crc = 0;

  while (offset < bytes.size() && !stop) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining >= sizeof(kFooterMagic) &&
        std::memcmp(bytes.data() + offset, kFooterMagic,
                    sizeof(kFooterMagic)) == 0) {
      if (remaining < kFooterSize) {
        result.truncated = true;
        result.error = "dump footer truncated";
        break;
      }
      if (!parse_footer(bytes.subspan(offset, kFooterSize), footer_count,
                        footer_records_crc, error)) {
        result.corrupted = true;
        result.error = error;
        break;
      }
      footer_seen = true;
      offset += kFooterSize;
      break;
    }
    if (remaining < kFrameHeaderSize) {
      result.truncated = true;
      result.error = "dump frame header truncated";
      break;
    }
    Decoder frame_header(bytes.subspan(offset, kFrameHeaderSize));
    const std::uint8_t type = frame_header.u8();
    const std::uint32_t payload_size = frame_header.u32();
    const std::uint32_t payload_crc = frame_header.u32();
    if (!frame_header.ok()) {
      result.corrupted = true;
      result.error = "malformed frame header";
      break;
    }
    if (payload_size > options.max_frame_bytes) {
      result.corrupted = true;
      result.error = "dump frame exceeds the size limit";
      break;
    }
    if (remaining - kFrameHeaderSize < payload_size) {
      result.truncated = true;
      result.error = "dump frame payload truncated";
      break;
    }
    const std::span<const std::byte> payload =
        bytes.subspan(offset + kFrameHeaderSize, payload_size);
    if (crc32c(payload) != payload_crc) {
      result.corrupted = true;
      result.error = "dump frame checksum mismatch";
      break;
    }
    offset += kFrameHeaderSize + payload_size;

    if (type == kFrameStrings) {
      if (strings_seen) {
        result.corrupted = true;
        result.error = "duplicate string table frame";
        break;
      }
      strings_seen = true;
      Decoder decoder(payload);
      const std::uint64_t count = decoder.varint();
      if (!decoder.ok() || count > StringTable::kMaxStrings) {
        result.corrupted = true;
        result.error = "invalid dump string table";
        break;
      }
      for (std::uint64_t i = 0; i < count; ++i) {
        char text[StringTable::kMaxStringBytes + 1];
        std::size_t length = 0;
        if (!decoder.string_into(text, sizeof(text), length)) {
          result.corrupted = true;
          result.error = "malformed dump string";
          break;
        }
        if (table.intern(std::string_view(text, length)) ==
            StringTable::kInvalidId) {
          result.corrupted = true;
          result.error = "dump string table exhausted";
          break;
        }
      }
      if (result.corrupted) {
        break;
      }
      if (!decoder.at_end()) {
        result.corrupted = true;
        result.error = "trailing bytes in string frame";
        break;
      }
    } else if (type == kFrameRecord) {
      Record record{};
      Decoder decoder(payload);
      if (!decode_record(decoder, record, inline_strings ? nullptr : &table) ||
          !decoder.at_end()) {
        result.corrupted = true;
        result.error = "malformed dump record";
        break;
      }
      if (record.seq == 0 || record.seq <= previous_seq) {
        result.corrupted = true;
        result.error = "dump records are not ordered by sequence";
        break;
      }
      if (result.records.size() >= options.max_records) {
        result.error = "dump contains too many records";
        stop = true;
        break;
      }
      previous_seq = record.seq;
      records_crc.update(payload);
      result.records.push_back(record);
    } else if (type == kFrameMeta) {
      // Reserved for future metadata. Skipped after CRC validation.
    } else {
      result.corrupted = true;
      result.error = "unknown dump frame type";
      break;
    }
  }

  result.consumed_bytes = offset;
  if (footer_seen) {
    if (footer_count != result.records.size()) {
      result.corrupted = true;
      result.error = "dump footer record count mismatch";
    } else if (footer_records_crc != records_crc.value()) {
      result.corrupted = true;
      result.error = "dump footer records checksum mismatch";
    } else if ((!inline_strings && header.string_count > 0 &&
                !strings_seen) ||
               (header.string_count != table.size() &&
                !(inline_strings && header.string_count == 0))) {
      result.corrupted = true;
      result.error = "dump footer string count mismatch";
    } else if (header.record_count != 0 &&
               header.record_count != result.records.size()) {
      result.corrupted = true;
      result.error = "dump header record count mismatch";
    } else if (offset != bytes.size()) {
      result.corrupted = true;
      result.error = "trailing bytes after dump footer";
    } else {
      result.complete = true;
    }
  } else if (!result.corrupted) {
    result.truncated = true;
    if (result.error.empty()) {
      result.error = "dump has no footer";
    }
  }

  if (result.complete) {
    result.ok = true;
  } else if (result.truncated || result.corrupted) {
    result.ok = !options.require_footer;
  }
  if (!result.ok && result.error.empty()) {
    result.error = "dump is incomplete";
  }
  return result;
}

DumpReadResult read_dump_file(std::string_view path,
                              const DumpReadOptions& options) {
  DumpReadResult result;
  std::vector<std::byte> bytes;
  std::uint64_t file_size = 0;
  std::string error;
  if (!read_entire_file(path, options.max_file_bytes, bytes, file_size,
                        error)) {
    result.error = error;
    return result;
  }
  result = read_dump_bytes(bytes, options);
  result.meta.file_size = file_size;
  return result;
}

bool read_dump_metadata(std::string_view path, DumpMetadata& out,
                        std::string* error) {
  std::string local_error;
  std::string& err = error != nullptr ? *error : local_error;
  err.clear();
  const std::string owned(path);
  const int fd = ::open(owned.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    err = std::string("cannot open dump: ") + std::strerror(errno);
    return false;
  }
  struct stat status {};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < static_cast<off_t>(kHeaderSize)) {
    err = "not a readable dump file";
    ::close(fd);
    return false;
  }
  std::array<std::byte, kHeaderSize> header{};
  std::size_t offset = 0;
  while (offset < header.size()) {
    const ssize_t got =
        ::read(fd, header.data() + offset, header.size() - offset);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      err = std::string("cannot read dump: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    offset += static_cast<std::size_t>(got);
  }
  ::close(fd);
  if (offset != header.size()) {
    err = "dump header truncated";
    return false;
  }
  ParsedHeader parsed;
  if (!parse_header(header, parsed, err)) {
    return false;
  }
  const bool has_footer = static_cast<std::uint64_t>(status.st_size) >=
                          static_cast<std::uint64_t>(kHeaderSize + kFooterSize);
  std::array<std::byte, kFooterSize> footer{};
  bool footer_valid = false;
  if (has_footer) {
    const int reopen = ::open(owned.c_str(), O_RDONLY | O_CLOEXEC);
    if (reopen >= 0) {
      const off_t footer_offset = static_cast<off_t>(status.st_size) -
                                  static_cast<off_t>(kFooterSize);
      std::size_t footer_read = 0;
      while (footer_read < footer.size()) {
        const ssize_t got = ::pread(reopen, footer.data() + footer_read,
                                    footer.size() - footer_read,
                                    footer_offset + static_cast<off_t>(footer_read));
        if (got < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        if (got == 0) {
          break;
        }
        footer_read += static_cast<std::size_t>(got);
      }
      ::close(reopen);
      if (footer_read == footer.size()) {
        std::uint64_t count = 0;
        std::uint32_t records_crc = 0;
        std::string footer_error;
        footer_valid = parse_footer(footer, count, records_crc, footer_error);
      }
    }
  }
  out = DumpMetadata{};
  out.version = static_cast<std::uint16_t>(parsed.version);
  out.flags = static_cast<std::uint16_t>(parsed.flags);
  out.created_wall_ns = parsed.created_wall_ns;
  out.record_count = parsed.record_count;
  out.first_seq = parsed.first_seq;
  out.last_seq = parsed.last_seq;
  out.string_count = parsed.string_count;
  out.file_size = static_cast<std::uint64_t>(status.st_size);
  out.footer_valid = footer_valid;
  return true;
}

std::vector<DumpFileInfo> list_dump_files(std::string_view directory,
                                          std::size_t limit) {
  std::vector<DumpFileInfo> dumps;
  if (directory.empty()) {
    return dumps;
  }
  std::error_code ec;
  std::filesystem::directory_iterator iterator{std::string(directory), ec};
  if (ec) {
    return dumps;
  }
  const std::filesystem::directory_iterator end;
  std::size_t scanned = 0;
  for (; iterator != end && scanned < 4096; iterator.increment(ec), ++scanned) {
    if (ec) {
      break;
    }
    const std::filesystem::directory_entry& entry = *iterator;
    std::error_code status_error;
    if (!entry.is_regular_file(status_error) || status_error) {
      continue;
    }
    if (entry.path().extension() != ".bbx") {
      continue;
    }
    DumpFileInfo info;
    info.path = entry.path().string();
    info.name = entry.path().filename().string();
    std::error_code size_error;
    info.size_bytes = static_cast<std::uint64_t>(entry.file_size(size_error));
    if (size_error) {
      info.size_bytes = 0;
    }
    std::string error;
    info.valid = read_dump_metadata(info.path, info.meta, &error);
    info.complete = info.valid && info.meta.footer_valid;
    info.error = error;
    dumps.push_back(std::move(info));
  }
  std::sort(dumps.begin(), dumps.end(),
            [](const DumpFileInfo& a, const DumpFileInfo& b) {
              if (a.valid != b.valid) {
                return a.valid;
              }
              if (a.meta.created_wall_ns != b.meta.created_wall_ns) {
                return a.meta.created_wall_ns > b.meta.created_wall_ns;
              }
              return a.name < b.name;
            });
  if (dumps.size() > limit) {
    dumps.resize(limit);
  }
  return dumps;
}

}  // namespace bb
