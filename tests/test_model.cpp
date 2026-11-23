#include "harness.hpp"

#include <blackbox/attribute.hpp>
#include <blackbox/ids.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

BB_TEST(trace_id_hex_roundtrip) {
  const bb::TraceId id = bb::TraceId::random();
  CHECK(!id.is_zero());
  const std::string hex = id.to_hex();
  CHECK_EQ(hex.size(), 32u);
  const auto parsed = bb::TraceId::from_hex(hex);
  CHECK(parsed.has_value());
  CHECK(*parsed == id);
}

BB_TEST(trace_id_rejects_bad_hex) {
  CHECK(!bb::TraceId::from_hex("").has_value());
  CHECK(!bb::TraceId::from_hex(std::string(31, 'a')).has_value());
  CHECK(!bb::TraceId::from_hex(std::string(32, 'z')).has_value());
  CHECK(!bb::TraceId::from_hex(std::string(32, '0')).has_value());
  CHECK(bb::TraceId::from_hex("00000000000000000000000000000001").has_value());
}

BB_TEST(trace_id_ordering_is_lexicographic) {
  const bb::TraceId a{1, 0};
  const bb::TraceId b{1, 1};
  const bb::TraceId c{2, 0};
  CHECK(a < b);
  CHECK(b < c);
  CHECK(!(c < a));
}

BB_TEST(trace_id_hex_layout) {
  const bb::TraceId id{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
  CHECK_EQ(id.to_hex(), std::string("0123456789abcdeffedcba9876543210"));
}

BB_TEST(span_and_correlation_ids_are_nonzero) {
  CHECK_NE(bb::new_correlation_id(), 0u);
  const bb::SpanId first = bb::next_span_id();
  const bb::SpanId second = bb::next_span_id();
  CHECK_NE(first, 0u);
  CHECK(second > first);
}

BB_TEST(attribute_keys_are_validated) {
  CHECK(bb::valid_attribute_key("db.query"));
  CHECK(bb::valid_attribute_key("http-status_code"));
  CHECK(bb::valid_attribute_key("a/b:c"));
  CHECK(bb::valid_attribute_key("A1"));
  CHECK(!bb::valid_attribute_key(""));
  CHECK(!bb::valid_attribute_key("has space"));
  CHECK(!bb::valid_attribute_key("quote\""));
  CHECK(!bb::valid_attribute_key(std::string(bb::kMaxAttributeKeyBytes + 1, 'a')));
}

BB_TEST(attribute_values_are_bounded) {
  CHECK(bb::valid_attribute_text("hello"));
  CHECK(bb::valid_attribute_text(std::string(bb::kMaxAttributeTextBytes, 'x')));
  CHECK(!bb::valid_attribute_text(std::string(bb::kMaxAttributeTextBytes + 1, 'x')));
  CHECK(!bb::valid_attribute_text(std::string_view("a\0b", 3)));

  bb::StoredValue stored;
  CHECK(!bb::store_value(
      bb::Value::string(std::string(bb::kMaxAttributeTextBytes + 1, 'x')), stored));
  CHECK(!bb::store_value(bb::Value::floating(std::numeric_limits<double>::quiet_NaN()),
                         stored));
  CHECK(!bb::store_value(
      bb::Value::floating(std::numeric_limits<double>::infinity()), stored));
}

BB_TEST(attribute_value_roundtrip) {
  const std::vector<bb::Value> values = {
      bb::Value::null(),
      bb::Value::boolean(true),
      bb::Value::integer(-42),
      bb::Value::floating(3.25),
      bb::Value::string("hello world"),
      bb::Value::string(""),
  };
  for (const bb::Value& value : values) {
    bb::StoredValue stored;
    CHECK(bb::store_value(value, stored));
    CHECK_EQ(static_cast<int>(stored.type), static_cast<int>(value.type()));
    const bb::Value owned = bb::owned_value(stored);
    CHECK(owned == value);
  }
}

BB_TEST(stored_attributes_keep_key_and_text) {
  bb::StoredAttribute stored;
  bb::store_key("db.query", stored);
  CHECK_EQ(bb::stored_key(stored), std::string_view("db.query"));
  bb::StoredValue value;
  CHECK(bb::store_value(bb::Value::string("SELECT 1"), value));
  stored.value = value;
  CHECK_EQ(bb::stored_text(stored.value), std::string_view("SELECT 1"));
  const bb::Attribute owned = bb::owned_attribute(stored);
  CHECK_EQ(owned.key, std::string("db.query"));
  CHECK_EQ(owned.value.as_string(), std::string("SELECT 1"));
  CHECK_EQ(owned.value.display(), std::string("SELECT 1"));
}

BB_TEST(value_display_is_stable) {
  CHECK_EQ(bb::Value::null().display(), std::string("null"));
  CHECK_EQ(bb::Value::boolean(false).display(), std::string("false"));
  CHECK_EQ(bb::Value::integer(-7).display(), std::string("-7"));
  CHECK_EQ(bb::Value::floating(1.5).display(), std::string("1.5"));
}
