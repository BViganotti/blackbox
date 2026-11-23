#pragma once

#include <blackbox/attribute.hpp>
#include <blackbox/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace bb {

/// What a record describes. Values are stable on disk; never renumber them.
enum class RecordKind : std::uint8_t {
  Log = 1,
  SpanBegin = 2,
  SpanEnd = 3,
  Metric = 4,
  StateChange = 5,
  Error = 6,
  Mark = 7,
  Note = 8,
};

enum class Level : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Fatal = 5,
};

enum class Status : std::uint8_t {
  Unset = 0,
  Ok = 1,
  Error = 2,
  Cancelled = 3,
  Timeout = 4,
  Rejected = 5,
};

inline constexpr std::size_t kMaxNameBytes = 63;
inline constexpr std::size_t kMaxMessageBytes = 127;
inline constexpr std::size_t kMaxStateKeyBytes = 27;

/// Upper bound for one stored record. The ring allocates slots of exactly this
/// size, so the memory budget is capacity * kRecordStorageBytes.
inline constexpr std::size_t kRecordStorageBytes = 2048;

const char* to_string(RecordKind kind) noexcept;
const char* to_string(Level level) noexcept;
const char* to_string(Status status) noexcept;
bool record_kind_from_string(std::string_view text, RecordKind& out) noexcept;
bool level_from_string(std::string_view text, Level& out) noexcept;
bool status_from_string(std::string_view text, Status& out) noexcept;

/// A fully self-contained observation. Trivially copyable, no heap storage, and
/// always safe to memcpy. Records are created by the recorder; the public
/// setters below never truncate.
///
/// `seq` is assigned when the record enters the ring (1-based). Zero means the
/// record has not been stored. `cause_seq` refers to the sequence number of the
/// record that causally preceded this one, or zero.
struct Record {
  std::uint64_t seq = 0;
  std::int64_t wall_ns = 0;
  std::int64_t mono_ns = 0;
  std::int64_t duration_ns = 0;
  std::int64_t int_value = 0;
  double double_value = 0.0;
  StoredValue value;
  TraceId trace;
  SpanId span = kNoSpan;
  SpanId parent = kNoSpan;
  CorrelationId correlation = kNoCorrelation;
  std::uint64_t cause_seq = 0;
  char name[kMaxNameBytes + 1] = {};
  char message[kMaxMessageBytes + 1] = {};
  char state_key[kMaxStateKeyBytes + 1] = {};
  StoredValue state_old;
  StoredValue state_new;
  StoredAttribute attrs[kMaxAttributesPerRecord];
  RecordKind kind = RecordKind::Log;
  Level level = Level::Info;
  Status status = Status::Unset;
  std::uint8_t attr_count = 0;
};

static_assert(std::is_trivially_copyable_v<Record>,
              "Record must remain trivially copyable");
static_assert(sizeof(Record) <= kRecordStorageBytes,
              "Record exceeds the ring slot budget");

std::string_view record_name(const Record& record) noexcept;
std::string_view record_message(const Record& record) noexcept;
std::string_view record_state_key(const Record& record) noexcept;

/// These setters reject NUL bytes and values that do not fit the field,
/// returning false without modifying the record.
bool set_record_name(Record& record, std::string_view text) noexcept;
bool set_record_message(Record& record, std::string_view text) noexcept;
bool set_record_state_key(Record& record, std::string_view text) noexcept;

/// Returns true when every field is internally consistent, every string is
/// NUL-terminated inside its buffer, all attributes are valid and unique, and
/// kind-specific invariants hold. If `error` is non-null it receives a short
/// reason on failure. Never reads outside the record.
bool validate_record(const Record& record, std::string* error = nullptr);

/// True for kinds that describe a point in time and do not carry state.
bool kind_is_instant(RecordKind kind) noexcept;

}  // namespace bb
