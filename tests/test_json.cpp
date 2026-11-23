#include "harness.hpp"
#include "support.hpp"

#include <blackbox/json.hpp>
#include <blackbox/present.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

BB_TEST(json_writer_emits_compact_structure) {
  bb::JsonWriter out;
  out.begin_object();
  out.key("a");
  out.number(std::int64_t{1});
  out.key("b");
  out.begin_array();
  out.boolean(true);
  out.null();
  out.string("x");
  out.end_array();
  out.end_object();
  CHECK(out.ok());
  CHECK_EQ(out.str(), std::string(R"({"a":1,"b":[true,null,"x"]})"));
}

BB_TEST(json_writer_escapes_and_validates_utf8) {
  bb::JsonWriter out;
  out.begin_object();
  out.key("text");
  out.string(std::string("a\"b\\c\n\t\x01"));
  out.end_object();
  CHECK(out.ok());
  CHECK_EQ(out.str(),
           std::string("{\"text\":\"a\\\"b\\\\c\\n\\t\\u0001\"}"));

  bb::JsonWriter invalid;
  std::string bad = "ok";
  bad.push_back(static_cast<char>(0x80));
  bad += "done";
  invalid.string(bad);
  CHECK(invalid.ok());
  CHECK(invalid.str().find("\xEF\xBF\xBD") != std::string::npos);

  bb::JsonWriter unicode;
  unicode.string("caf\xC3\xA9 \xF0\x9F\x9A\x80");
  CHECK(unicode.ok());
  CHECK_EQ(unicode.str(), std::string("\"caf\xC3\xA9 \xF0\x9F\x9A\x80\""));
}

BB_TEST(json_writer_rejects_nonfinite_and_misuse) {
  bb::JsonWriter nan_writer;
  nan_writer.number(std::numeric_limits<double>::quiet_NaN());
  CHECK(!nan_writer.ok());

  bb::JsonWriter key_outside;
  key_outside.key("x");
  CHECK(!key_outside.ok());

  bb::JsonWriter two_roots;
  two_roots.null();
  two_roots.boolean(false);
  CHECK(!two_roots.ok());

  bb::JsonWriter bad_end;
  bad_end.begin_object();
  bad_end.end_array();
  CHECK(!bad_end.ok());

  bb::JsonWriter missing_value;
  missing_value.begin_object();
  missing_value.key("x");
  missing_value.end_object();
  CHECK(!missing_value.ok());
}

BB_TEST(json_writer_enforces_byte_budget_and_depth) {
  bb::JsonWriter small(8);
  small.begin_array();
  small.string("this is too long for the budget");
  CHECK(!small.ok());

  bb::JsonWriter deep;
  for (std::size_t i = 0; i < bb::JsonWriter::kMaxDepth + 2; ++i) {
    deep.begin_array();
  }
  CHECK(!deep.ok());
}

BB_TEST(json_record_contains_all_fields) {
  const bb::Record record = bbtest::sample_record(3);
  bb::JsonWriter out;
  bb::write_record_json(out, record);
  CHECK(out.ok());
  const std::string& text = out.str();
  CHECK(text.find("\"seq\":3") != std::string::npos);
  CHECK(text.find("\"kind\":\"state_change\"") != std::string::npos);
  CHECK(text.find("\"status\":\"unset\"") != std::string::npos);
  CHECK(text.find("\"attrs\":{") != std::string::npos);
  CHECK(text.find("\"index\":3") != std::string::npos);
  CHECK(text.find("\"state_key\":\"state.key\"") != std::string::npos);
  CHECK(text.find("\"state_old\":\"old\"") != std::string::npos);
  CHECK(text.find("\"state_new\":\"new\"") != std::string::npos);
  CHECK(text.find("\"trace\":\"1000000000000003") != std::string::npos);
  CHECK(text.front() == '{');
  CHECK(text.back() == '}');
}

BB_TEST(json_stats_and_value_types) {
  bb::Ring ring(16);
  CHECK(ring.push(bbtest::sample_record(1)));
  const bb::RingStats stats = ring.stats();
  bb::JsonWriter out;
  bb::write_ring_stats_json(out, stats);
  CHECK(out.ok());
  CHECK(out.str().find("\"capacity\":16") != std::string::npos);
  CHECK(out.str().find("\"produced\":1") != std::string::npos);

  bb::JsonWriter values;
  values.begin_array();
  bb::write_value_json(values, bb::Value::null());
  bb::write_value_json(values, bb::Value::boolean(true));
  bb::write_value_json(values, bb::Value::integer(-5));
  bb::write_value_json(values, bb::Value::floating(1.25));
  bb::write_value_json(values, bb::Value::string("text"));
  values.end_array();
  CHECK(values.ok());
  CHECK_EQ(values.str(), std::string("[null,true,-5,1.25,\"text\"]"));
}

BB_TEST(json_writer_budget_failure_is_reported) {
  bb::JsonWriter writer;
  writer.begin_object();
  writer.key("message");
  writer.string("line\nbreak");
  writer.end_object();
  CHECK(writer.ok());
  CHECK(writer.str().find('\n') == std::string::npos);
  CHECK(writer.str().find("\\n") != std::string::npos);

  bb::JsonWriter big(64);
  big.begin_array();
  for (int i = 0; i < 100; ++i) {
    big.number(std::int64_t{i});
  }
  CHECK(!big.ok());
  CHECK(big.str().size() <= 64u);
  CHECK(big.str().front() == '[');
}
