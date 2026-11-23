#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bb {

/// Bounded JSON emitter. Every write is checked against a hard byte budget; on
/// overflow or API misuse (a key outside an object, a second root value, too
/// much nesting) ok() becomes false and further writes are ignored. The writer
/// never produces invalid JSON on a successful path.
///
/// Strings are escaped with strict UTF-8 validation; invalid bytes are replaced
/// by U+FFFD. Non-finite numbers are rejected rather than emitted as
/// non-standard NaN/Infinity.
class JsonWriter {
 public:
  static constexpr std::size_t kMaxDepth = 32;
  static constexpr std::size_t kDefaultMaxBytes = 8u * 1024u * 1024u;

  explicit JsonWriter(std::size_t max_bytes = kDefaultMaxBytes);

  bool ok() const noexcept { return ok_; }
  const std::string& str() const noexcept { return out_; }

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();
  void key(std::string_view name);
  void string(std::string_view text);
  void number(std::int64_t value);
  void number(std::uint64_t value);
  void number(double value);
  void boolean(bool value);
  void null();

 private:
  struct Level {
    bool object = false;
    bool first = true;
    bool expecting_key = false;
  };

  void prepare_value();
  void append(std::string_view text);
  void append_byte(char c);
  void write_escaped(std::string_view text);

  std::string out_;
  std::size_t max_bytes_ = 0;
  std::vector<Level> stack_;
  bool root_written_ = false;
  bool ok_ = true;
};

}  // namespace bb
