#include <blackbox/attribute.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace bb {

const char* to_string(ValueType type) noexcept {
  switch (type) {
    case ValueType::Null:
      return "null";
    case ValueType::Bool:
      return "bool";
    case ValueType::Int:
      return "int";
    case ValueType::Float:
      return "float";
    case ValueType::String:
      return "string";
  }
  return "invalid";
}

bool value_type_from_string(std::string_view text, ValueType& out) noexcept {
  if (text == "null") {
    out = ValueType::Null;
  } else if (text == "bool") {
    out = ValueType::Bool;
  } else if (text == "int") {
    out = ValueType::Int;
  } else if (text == "float") {
    out = ValueType::Float;
  } else if (text == "string") {
    out = ValueType::String;
  } else {
    return false;
  }
  return true;
}

Value Value::null() { return Value(); }

Value Value::boolean(bool value) {
  Value out;
  out.type_ = ValueType::Bool;
  out.boolean_ = value;
  return out;
}

Value Value::integer(std::int64_t value) {
  Value out;
  out.type_ = ValueType::Int;
  out.integer_ = value;
  return out;
}

Value Value::floating(double value) {
  Value out;
  out.type_ = ValueType::Float;
  out.real_ = value;
  return out;
}

Value Value::string(std::string value) {
  Value out;
  out.type_ = ValueType::String;
  out.text_ = std::move(value);
  return out;
}

std::string Value::display() const {
  switch (type_) {
    case ValueType::Null:
      return "null";
    case ValueType::Bool:
      return boolean_ ? "true" : "false";
    case ValueType::Int:
      return std::to_string(integer_);
    case ValueType::Float: {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.6g", real_);
      return buffer;
    }
    case ValueType::String:
      return text_;
  }
  return "null";
}

bool Value::operator==(const Value& other) const noexcept {
  if (type_ != other.type_) {
    return false;
  }
  switch (type_) {
    case ValueType::Null:
      return true;
    case ValueType::Bool:
      return boolean_ == other.boolean_;
    case ValueType::Int:
      return integer_ == other.integer_;
    case ValueType::Float:
      return real_ == other.real_;
    case ValueType::String:
      return text_ == other.text_;
  }
  return false;
}

bool valid_attribute_key(std::string_view key) noexcept {
  if (key.empty() || key.size() > kMaxAttributeKeyBytes) {
    return false;
  }
  const auto boundary = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  if (!boundary(key.front()) || !boundary(key.back())) {
    return false;
  }
  for (char c : key) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9');
    if (!alnum && c != '.' && c != '_' && c != '-' && c != ':' && c != '/') {
      return false;
    }
  }
  return true;
}

bool valid_attribute_text(std::string_view text) noexcept {
  if (text.size() > kMaxAttributeTextBytes) {
    return false;
  }
  return text.find('\0') == std::string_view::npos;
}

bool store_value(const Value& value, StoredValue& out) noexcept {
  StoredValue candidate;
  candidate.type = value.type();
  switch (value.type()) {
    case ValueType::Null:
      break;
    case ValueType::Bool:
      candidate.boolean = value.as_bool();
      break;
    case ValueType::Int:
      candidate.integer = value.as_int();
      break;
    case ValueType::Float:
      if (!std::isfinite(value.as_double())) {
        return false;
      }
      candidate.real = value.as_double();
      break;
    case ValueType::String: {
      const std::string& text = value.as_string();
      if (!valid_attribute_text(text)) {
        return false;
      }
      candidate.text_length = static_cast<std::uint16_t>(text.size());
      if (!text.empty()) {
        std::memcpy(candidate.text, text.data(), text.size());
      }
      candidate.text[text.size()] = '\0';
      break;
    }
  }
  out = candidate;
  return true;
}

void store_key(std::string_view key, StoredAttribute& out) noexcept {
  const std::size_t length = key.size() < kMaxAttributeKeyBytes
                                 ? key.size()
                                 : kMaxAttributeKeyBytes;
  std::memcpy(out.key, key.data(), length);
  out.key[length] = '\0';
}

Value owned_value(const StoredValue& value) {
  switch (value.type) {
    case ValueType::Null:
      return Value::null();
    case ValueType::Bool:
      return Value::boolean(value.boolean);
    case ValueType::Int:
      return Value::integer(value.integer);
    case ValueType::Float:
      return Value::floating(value.real);
    case ValueType::String:
      return Value::string(std::string(stored_text(value)));
  }
  return Value::null();
}

Attribute owned_attribute(const StoredAttribute& attribute) {
  return Attribute{std::string(stored_key(attribute)), owned_value(attribute.value)};
}

std::string_view stored_text(const StoredValue& value) noexcept {
  const std::size_t length =
      value.text_length <= kMaxAttributeTextBytes ? value.text_length
                                                  : kMaxAttributeTextBytes;
  return std::string_view(value.text, length);
}

std::string_view stored_key(const StoredAttribute& attribute) noexcept {
  std::size_t length = 0;
  while (length <= kMaxAttributeKeyBytes && attribute.key[length] != '\0') {
    ++length;
  }
  return std::string_view(attribute.key, length);
}

}  // namespace bb
