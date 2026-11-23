#pragma once

#include <blackbox/record.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bb {

/// On-disk flags. Unknown bits are rejected.
inline constexpr std::uint16_t kDumpFlagInlineStrings = 1u << 0;
inline constexpr std::uint16_t kDumpFlagCrash = 1u << 1;
inline constexpr std::uint16_t kDumpFlagPartial = 1u << 2;
inline constexpr std::uint16_t kDumpKnownFlags =
    kDumpFlagInlineStrings | kDumpFlagCrash | kDumpFlagPartial;

struct DumpMetadata {
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::int64_t created_wall_ns = 0;
  std::uint64_t record_count = 0;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  std::uint32_t string_count = 0;
  std::uint64_t file_size = 0;
};

struct DumpWriteOptions {
  /// Inline every string instead of writing a string table. Used by the
  /// async-signal-safe crash writer.
  bool inline_strings = false;
  /// Extra flag bits (kDumpFlagCrash, kDumpFlagPartial). Unknown bits are
  /// rejected.
  std::uint16_t extra_flags = 0;
  /// Zero means "now". Tests set this for byte-stable golden files.
  std::int64_t created_wall_ns = 0;
};

/// Writes a complete dump atomically: temp file, fsync, rename. Records must be
/// valid and strictly increasing by sequence, at most 1,000,000 records.
/// Returns false and fills `error` (when non-null) on any failure. The final
/// path never contains a partial dump.
bool write_dump_file(std::string_view path, std::span<const Record> records,
                     const DumpWriteOptions& options = {},
                     std::string* error = nullptr);

struct DumpReadOptions {
  /// When true, a missing or invalid footer, bad checksum or trailing bytes is
  /// an error. When false, everything parsed before the damage is returned and
  /// the result flags say what went wrong.
  bool require_footer = true;
  std::size_t max_records = 1'000'000;
  std::size_t max_file_bytes = 256u * 1024u * 1024u;
  std::uint32_t max_frame_bytes = 8u * 1024u * 1024u;
};

struct DumpReadResult {
  /// True when the header was valid and the requested strictness was met.
  bool ok = false;
  /// True when a valid footer was seen and all checksums matched.
  bool complete = false;
  bool truncated = false;
  bool corrupted = false;
  std::string error;
  std::size_t consumed_bytes = 0;
  DumpMetadata meta;
  std::vector<Record> records;
};

/// Parses a dump held in memory. This is the bounded entry point used by fuzz
/// tests. Never allocates beyond the record and frame limits.
DumpReadResult read_dump_bytes(std::span<const std::byte> bytes,
                               const DumpReadOptions& options = {});

/// Loads and parses a dump file. Files above options.max_file_bytes are
/// rejected without being read.
DumpReadResult read_dump_file(std::string_view path,
                              const DumpReadOptions& options = {});

/// Reads only the validated header and file size. Useful for dump listings.
bool read_dump_metadata(std::string_view path, DumpMetadata& out,
                        std::string* error = nullptr);

struct DumpFileInfo {
  std::string path;
  std::string name;
  std::uint64_t size_bytes = 0;
  bool valid = false;
  std::string error;
  DumpMetadata meta;
};

/// Lists *.bbx files in `directory` (bounded scan), newest first. Unreadable or
/// corrupt files are included with valid=false so the UI can explain them.
std::vector<DumpFileInfo> list_dump_files(std::string_view directory,
                                          std::size_t limit = 128);

}  // namespace bb
