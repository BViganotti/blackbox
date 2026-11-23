#include <blackbox/json.hpp>

#include <cmath>
#include <cstdio>

namespace bb {
namespace {

// Returns the length of a valid UTF-8 sequence starting at `text[index]`, or 0
// when the bytes are not valid UTF-8.
std::size_t utf8_length(std::string_view text, std::size_t index) {
  const auto byte = [&](std::size_t offset) {
    return static_cast<unsigned char>(text[index + offset]);
  };
  const unsigned char first = byte(0);
  if (first < 0x80) {
    return 1;
  }
  if (first >= 0xC2 && first <= 0xDF) {
    if (index + 1 < text.size() && (byte(1) & 0xC0) == 0x80) {
      return 2;
    }
    return 0;
  }
  if (first >= 0xE0 && first <= 0xEF) {
    if (index + 2 >= text.size()) {
      return 0;
    }
    const unsigned char second = byte(1);
    const unsigned char third = byte(2);
    if ((third & 0xC0) != 0x80 || (second & 0xC0) != 0x80) {
      return 0;
    }
    if (first == 0xE0 && second < 0xA0) {
      return 0;
    }
    if (first == 0xED && second >= 0xA0) {
      return 0;  // surrogate range
    }
    return 3;
  }
  if (first >= 0xF0 && first <= 0xF4) {
    if (index + 3 >= text.size()) {
      return 0;
    }
    const unsigned char second = byte(1);
    const unsigned char third = byte(2);
    const unsigned char fourth = byte(3);
    if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 ||
        (fourth & 0xC0) != 0x80) {
      return 0;
    }
    if (first == 0xF0 && second < 0x90) {
      return 0;
    }
    if (first == 0xF4 && second > 0x8F) {
      return 0;
    }
    return 4;
  }
  return 0;
}

}  // namespace

JsonWriter::JsonWriter(std::size_t max_bytes)
    : max_bytes_(max_bytes == 0 ? 1 : max_bytes) {
  stack_.reserve(8);
}

void JsonWriter::append(std::string_view text) {
  if (!ok_) {
    return;
  }
  if (text.size() > max_bytes_ || out_.size() > max_bytes_ - text.size()) {
    ok_ = false;
    return;
  }
  out_.append(text);
}

void JsonWriter::append_byte(char c) {
  if (!ok_) {
    return;
  }
  if (out_.size() >= max_bytes_) {
    ok_ = false;
    return;
  }
  out_.push_back(c);
}

void JsonWriter::prepare_value() {
  if (!ok_) {
    return;
  }
  if (stack_.empty()) {
    if (root_written_) {
      ok_ = false;
      return;
    }
    root_written_ = true;
    return;
  }
  Level& level = stack_.back();
  if (level.object) {
    if (!level.expecting_key) {
      ok_ = false;
      return;
    }
    level.expecting_key = false;
    return;
  }
  if (!level.first) {
    append_byte(',');
  }
  level.first = false;
}

void JsonWriter::begin_object() {
  prepare_value();
  if (!ok_) {
    return;
  }
  if (stack_.size() >= kMaxDepth) {
    ok_ = false;
    return;
  }
  append_byte('{');
  stack_.push_back(Level{true, true, false});
}

void JsonWriter::end_object() {
  if (!ok_ || stack_.empty() || !stack_.back().object ||
      stack_.back().expecting_key) {
    ok_ = false;
    return;
  }
  stack_.pop_back();
  append_byte('}');
}

void JsonWriter::begin_array() {
  prepare_value();
  if (!ok_) {
    return;
  }
  if (stack_.size() >= kMaxDepth) {
    ok_ = false;
    return;
  }
  append_byte('[');
  stack_.push_back(Level{false, true, false});
}

void JsonWriter::end_array() {
  if (!ok_ || stack_.empty() || stack_.back().object) {
    ok_ = false;
    return;
  }
  stack_.pop_back();
  append_byte(']');
}

void JsonWriter::key(std::string_view name) {
  if (!ok_ || stack_.empty() || !stack_.back().object ||
      stack_.back().expecting_key) {
    ok_ = false;
    return;
  }
  Level& level = stack_.back();
  if (!level.first) {
    append_byte(',');
  }
  level.first = false;
  write_escaped(name);
  append_byte(':');
  level.expecting_key = true;
}

void JsonWriter::string(std::string_view text) {
  prepare_value();
  if (!ok_) {
    return;
  }
  write_escaped(text);
}

void JsonWriter::number(std::int64_t value) {
  prepare_value();
  if (!ok_) {
    return;
  }
  char buffer[32];
  const int length = std::snprintf(buffer, sizeof(buffer), "%lld",
                                   static_cast<long long>(value));
  if (length <= 0) {
    ok_ = false;
    return;
  }
  append(std::string_view(buffer, static_cast<std::size_t>(length)));
}

void JsonWriter::number(std::uint64_t value) {
  prepare_value();
  if (!ok_) {
    return;
  }
  char buffer[32];
  const int length = std::snprintf(buffer, sizeof(buffer), "%llu",
                                   static_cast<unsigned long long>(value));
  if (length <= 0) {
    ok_ = false;
    return;
  }
  append(std::string_view(buffer, static_cast<std::size_t>(length)));
}

void JsonWriter::number(double value) {
  prepare_value();
  if (!ok_) {
    return;
  }
  if (!std::isfinite(value)) {
    ok_ = false;
    return;
  }
  char buffer[40];
  const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (length <= 0) {
    ok_ = false;
    return;
  }
  append(std::string_view(buffer, static_cast<std::size_t>(length)));
}

void JsonWriter::boolean(bool value) {
  prepare_value();
  if (!ok_) {
    return;
  }
  append(value ? "true" : "false");
}

void JsonWriter::null() {
  prepare_value();
  if (!ok_) {
    return;
  }
  append("null");
}

void JsonWriter::write_escaped(std::string_view text) {
  append_byte('"');
  std::size_t index = 0;
  while (index < text.size() && ok_) {
    const unsigned char byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80) {
      switch (byte) {
        case '"':
          append("\\\"");
          break;
        case '\\':
          append("\\\\");
          break;
        case '\b':
          append("\\b");
          break;
        case '\f':
          append("\\f");
          break;
        case '\n':
          append("\\n");
          break;
        case '\r':
          append("\\r");
          break;
        case '\t':
          append("\\t");
          break;
        default:
          if (byte < 0x20) {
            char buffer[7];
            const int length = std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                             static_cast<unsigned>(byte));
            if (length > 0) {
              append(std::string_view(buffer, static_cast<std::size_t>(length)));
            }
          } else {
            append_byte(text[index]);
          }
          break;
      }
      ++index;
      continue;
    }
    const std::size_t length = utf8_length(text, index);
    if (length == 0) {
      append("\xEF\xBF\xBD");
      ++index;
    } else {
      append(text.substr(index, length));
      index += length;
    }
  }
  append_byte('"');
}

}  // namespace bb
