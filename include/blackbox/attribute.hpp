#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bb {

/// Hard bounds for attributes. Values outside these bounds are rejected by the
/// recorder; the library never silently truncates.
inline constexpr std::size_t kMaxAttributeKeyBytes = 27;
inline constexpr std::size_t kMaxAttributeTextBytes = 96;
inline constexpr std::size_t kMaxAttributesPerRecord = 8;

enum class ValueType : std::uint8_t {
  Null = 0,
  Bool = 1,
  Int = 2,
  Float = 3,
  String = 4,
};

const char* to_string(ValueType type) noexcept;
bool value_type_from_string(std::string_view text, ValueType& out) noexcept;

/// Owned attribute value used by callers. Strings are bounded only by the
/// recorder's limits at record time.
class Value {
 public:
  Value() = default;
  static Value null();
  static Value boolean(bool value);
  static Value integer(std::int64_t value);
  static Value floating(double value);
  static Value string(std::string value);

  ValueType type() const noexcept { return type_; }
  bool is_null() const noexcept { return type_ == ValueType::Null; }
  bool as_bool() const noexcept { return boolean_; }
  std::int64_t as_int() const noexcept { return integer_; }
  double as_double() const noexcept { return real_; }
  const std::string& as_string() const noexcept { return text_; }

  /// Human readable rendering used by the UI and by log-style output.
  std::string display() const;
  bool operator==(const Value& other) const noexcept;

 private:
  ValueType type_ = ValueType::Null;
  bool boolean_ = false;
  std::int64_t integer_ = 0;
  double real_ = 0.0;
  std::string text_;
};

/// A key/value attribute supplied by the caller.
struct Attribute {
  std::string key;
  Value value;
};

/// Fixed-size stored value. Trivially copyable so records can live in the ring
/// without allocating.
struct StoredValue {
  ValueType type = ValueType::Null;
  bool boolean = false;
  std::uint16_t text_length = 0;
  std::int64_t integer = 0;
  double real = 0.0;
  char text[kMaxAttributeTextBytes + 1] = {};
};

struct StoredAttribute {
  char key[kMaxAttributeKeyBytes + 1] = {};
  StoredValue value;
};

/// Returns true if `key` is non-empty, at most kMaxAttributeKeyBytes long, and
/// contains only ASCII letters, digits, '.', '_', '-', ':' or '/'.
bool valid_attribute_key(std::string_view key) noexcept;

/// Returns true if `text` contains no NUL byte and is at most
/// kMaxAttributeTextBytes bytes.
bool valid_attribute_text(std::string_view text) noexcept;

/// Stores `value` into `out`. Returns false (leaving `out` untouched) when the
/// value is too large or a floating point value is not finite.
bool store_value(const Value& value, StoredValue& out) noexcept;

/// Copies `key` into `out`. The caller must have validated it.
void store_key(std::string_view key, StoredAttribute& out) noexcept;

/// Converts a stored attribute to an owned Value/Attribute pair.
Value owned_value(const StoredValue& value);
Attribute owned_attribute(const StoredAttribute& attribute);

/// Views over stored text. Safe because the stored buffers are always
/// NUL-terminated and the length is bounded.
std::string_view stored_text(const StoredValue& value) noexcept;
std::string_view stored_key(const StoredAttribute& attribute) noexcept;

}  // namespace bb
