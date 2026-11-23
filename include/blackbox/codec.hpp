#pragma once

#include <blackbox/record.hpp>
#include <blackbox/string_table.hpp>
#include <blackbox/util/varint.hpp>

#include <cstddef>
#include <span>

namespace bb {

/// Largest encoded record the codec will produce or accept. Callers should
/// allocate at least this many bytes when encoding.
inline constexpr std::size_t kMaxEncodedRecordBytes = 8192;

/// Encodes `record` into `encoder`. When `table` is non-null, strings are
/// replaced by ids and interned into the table; otherwise they are inlined.
/// Returns false if the record is invalid, a string cannot be interned, or the
/// output buffer is too small. On failure the encoder is left in a failed state
/// and must be discarded.
bool encode_record(const Record& record, Encoder& encoder, StringTable* table);

/// Decodes one record. When `table` is non-null, ids are resolved through it;
/// otherwise inline strings are read. Performs full validation before writing
/// `out`; `out` is untouched on failure. Returns false on any malformed,
/// truncated or out-of-range input.
bool decode_record(Decoder& decoder, Record& out, const StringTable* table);

/// Convenience wrappers over a fixed buffer.
bool encode_record_to(const Record& record, std::span<std::byte> buffer,
                      StringTable* table, std::size_t& written);
bool decode_record_from(std::span<const std::byte> bytes, Record& out,
                        const StringTable* table);

}  // namespace bb
