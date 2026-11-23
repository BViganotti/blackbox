#include <blackbox/record.hpp>

#include <cmath>
#include <cstring>

namespace bb {
namespace {

template <std::size_t N>
std::string_view bounded_view(const char (&data)[N]) noexcept {
  std::size_t length = 0;
  while (length < N && data[length] != '\0') {
    ++length;
  }
  return std::string_view(data, length);
}

template <std::size_t N>
bool set_bounded(char (&data)[N], std::string_view text) noexcept {
  if (text.size() >= N || text.find('\0') != std::string_view::npos) {
    return false;
  }
  if (!text.empty()) {
    std::memcpy(data, text.data(), text.size());
  }
  data[text.size()] = '\0';
  return true;
}

template <std::size_t N>
bool terminated(const char (&data)[N]) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (data[i] == '\0') {
      return true;
    }
  }
  return false;
}

bool valid_stored_value(const StoredValue& value) {
  switch (value.type) {
    case ValueType::Null:
    case ValueType::Bool:
    case ValueType::Int:
      return true;
    case ValueType::Float:
      return std::isfinite(value.real);
    case ValueType::String: {
      if (value.text_length > kMaxAttributeTextBytes) {
        return false;
      }
      for (std::size_t i = 0; i < value.text_length; ++i) {
        if (value.text[i] == '\0') {
          return false;
        }
      }
      return value.text[value.text_length] == '\0';
    }
  }
  return false;
}

bool set_error(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

const char* to_string(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::Log:
      return "log";
    case RecordKind::SpanBegin:
      return "span_begin";
    case RecordKind::SpanEnd:
      return "span_end";
    case RecordKind::Metric:
      return "metric";
    case RecordKind::StateChange:
      return "state_change";
    case RecordKind::Error:
      return "error";
    case RecordKind::Mark:
      return "mark";
    case RecordKind::Note:
      return "note";
  }
  return "invalid";
}

const char* to_string(Level level) noexcept {
  switch (level) {
    case Level::Trace:
      return "trace";
    case Level::Debug:
      return "debug";
    case Level::Info:
      return "info";
    case Level::Warn:
      return "warn";
    case Level::Error:
      return "error";
    case Level::Fatal:
      return "fatal";
  }
  return "invalid";
}

const char* to_string(Status status) noexcept {
  switch (status) {
    case Status::Unset:
      return "unset";
    case Status::Ok:
      return "ok";
    case Status::Error:
      return "error";
    case Status::Cancelled:
      return "cancelled";
    case Status::Timeout:
      return "timeout";
    case Status::Rejected:
      return "rejected";
  }
  return "invalid";
}

bool record_kind_from_string(std::string_view text, RecordKind& out) noexcept {
  if (text == "log") {
    out = RecordKind::Log;
  } else if (text == "span_begin" || text == "span") {
    out = RecordKind::SpanBegin;
  } else if (text == "span_end") {
    out = RecordKind::SpanEnd;
  } else if (text == "metric") {
    out = RecordKind::Metric;
  } else if (text == "state_change" || text == "state") {
    out = RecordKind::StateChange;
  } else if (text == "error") {
    out = RecordKind::Error;
  } else if (text == "mark") {
    out = RecordKind::Mark;
  } else if (text == "note") {
    out = RecordKind::Note;
  } else {
    return false;
  }
  return true;
}

bool level_from_string(std::string_view text, Level& out) noexcept {
  if (text == "trace") {
    out = Level::Trace;
  } else if (text == "debug") {
    out = Level::Debug;
  } else if (text == "info") {
    out = Level::Info;
  } else if (text == "warn" || text == "warning") {
    out = Level::Warn;
  } else if (text == "error") {
    out = Level::Error;
  } else if (text == "fatal") {
    out = Level::Fatal;
  } else {
    return false;
  }
  return true;
}

bool status_from_string(std::string_view text, Status& out) noexcept {
  if (text == "unset") {
    out = Status::Unset;
  } else if (text == "ok") {
    out = Status::Ok;
  } else if (text == "error") {
    out = Status::Error;
  } else if (text == "cancelled" || text == "canceled") {
    out = Status::Cancelled;
  } else if (text == "timeout") {
    out = Status::Timeout;
  } else if (text == "rejected") {
    out = Status::Rejected;
  } else {
    return false;
  }
  return true;
}

std::string_view record_name(const Record& record) noexcept {
  return bounded_view(record.name);
}

std::string_view record_message(const Record& record) noexcept {
  return bounded_view(record.message);
}

std::string_view record_state_key(const Record& record) noexcept {
  return bounded_view(record.state_key);
}

bool set_record_name(Record& record, std::string_view text) noexcept {
  return set_bounded(record.name, text);
}

bool set_record_message(Record& record, std::string_view text) noexcept {
  return set_bounded(record.message, text);
}

bool set_record_state_key(Record& record, std::string_view text) noexcept {
  return set_bounded(record.state_key, text);
}

bool kind_is_instant(RecordKind kind) noexcept {
  return kind == RecordKind::Log || kind == RecordKind::Metric ||
         kind == RecordKind::Error || kind == RecordKind::Mark ||
         kind == RecordKind::Note;
}

bool validate_record(const Record& record, std::string* error) {
  switch (record.kind) {
    case RecordKind::Log:
    case RecordKind::SpanBegin:
    case RecordKind::SpanEnd:
    case RecordKind::Metric:
    case RecordKind::StateChange:
    case RecordKind::Error:
    case RecordKind::Mark:
    case RecordKind::Note:
      break;
    default:
      return set_error(error, "unknown record kind");
  }
  if (record.level > Level::Fatal) {
    return set_error(error, "unknown level");
  }
  if (record.status > Status::Rejected) {
    return set_error(error, "unknown status");
  }
  if (record.attr_count > kMaxAttributesPerRecord) {
    return set_error(error, "attribute count out of range");
  }
  if (!terminated(record.name) || !terminated(record.message) ||
      !terminated(record.state_key)) {
    return set_error(error, "unterminated fixed string");
  }
  if (record.duration_ns < 0) {
    return set_error(error, "negative duration");
  }
  if (record.kind == RecordKind::SpanBegin || record.kind == RecordKind::SpanEnd) {
    if (record.trace.is_zero()) {
      return set_error(error, "span record without trace id");
    }
    if (record.span == kNoSpan) {
      return set_error(error, "span record without span id");
    }
    if (record.kind == RecordKind::SpanBegin && record_name(record).empty()) {
      return set_error(error, "span begin without name");
    }
  }
  if (record.kind == RecordKind::Metric && record_name(record).empty()) {
    return set_error(error, "metric without name");
  }
  if (record.kind == RecordKind::StateChange) {
    if (record_state_key(record).empty()) {
      return set_error(error, "state change without key");
    }
  } else if (!record_state_key(record).empty()) {
    return set_error(error, "state key on a non-state record");
  }
  if (!valid_stored_value(record.value) || !valid_stored_value(record.state_old) ||
      !valid_stored_value(record.state_new)) {
    return set_error(error, "invalid stored value");
  }
  if (!std::isfinite(record.double_value)) {
    return set_error(error, "non-finite record value");
  }
  for (std::size_t i = 0; i < record.attr_count; ++i) {
    const StoredAttribute& attr = record.attrs[i];
    if (!terminated(attr.key)) {
      return set_error(error, "unterminated attribute key");
    }
    const std::string_view key = stored_key(attr);
    if (!valid_attribute_key(key)) {
      return set_error(error, "invalid attribute key");
    }
    for (std::size_t j = i + 1; j < record.attr_count; ++j) {
      if (stored_key(record.attrs[j]) == key) {
        return set_error(error, "duplicate attribute key");
      }
    }
    if (!valid_stored_value(attr.value)) {
      return set_error(error, "invalid attribute value");
    }
    if (attr.value.type == ValueType::String && !terminated(attr.value.text)) {
      return set_error(error, "unterminated attribute text");
    }
  }
  return true;
}

}  // namespace bb
