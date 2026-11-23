#include "harness.hpp"

#include <blackbox/query.hpp>
#include <blackbox/record.hpp>
#include <blackbox/util/random.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

bb::Record log_record() {
  bb::Record record{};
  record.seq = 10;
  record.kind = bb::RecordKind::Log;
  record.level = bb::Level::Info;
  record.trace = bb::TraceId{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
  record.span = 7;
  record.correlation = 0xCAFE;
  record.cause_seq = 3;
  record.wall_ns = 1'700'000'000'000'000'000ll;
  record.mono_ns = 1'000;
  bb::set_record_name(record, "db.query");
  bb::set_record_message(record, "database timeout while waiting");
  return record;
}

bb::Record error_record() {
  bb::Record record = log_record();
  record.seq = 20;
  record.kind = bb::RecordKind::Error;
  record.level = bb::Level::Error;
  record.status = bb::Status::Timeout;
  record.duration_ns = 250'000'000;
  bb::set_record_message(record, "connection failed");
  record.attr_count = 4;
  bb::store_key("http.status", record.attrs[0]);
  (void)bb::store_value(bb::Value::integer(500), record.attrs[0].value);
  bb::store_key("db.system", record.attrs[1]);
  (void)bb::store_value(bb::Value::string("postgres"), record.attrs[1].value);
  bb::store_key("retry", record.attrs[2]);
  (void)bb::store_value(bb::Value::boolean(true), record.attrs[2].value);
  bb::store_key("ratio", record.attrs[3]);
  (void)bb::store_value(bb::Value::floating(0.25), record.attrs[3].value);
  return record;
}

bb::Record state_record() {
  bb::Record record = log_record();
  record.seq = 30;
  record.kind = bb::RecordKind::StateChange;
  record.level = bb::Level::Warn;
  bb::set_record_state_key(record, "breaker.state");
  (void)bb::store_value(bb::Value::string("closed"), record.state_old);
  (void)bb::store_value(bb::Value::string("open"), record.state_new);
  bb::set_record_message(record, "breaker opened");
  return record;
}

bb::Record metric_record() {
  bb::Record record = log_record();
  record.seq = 40;
  record.kind = bb::RecordKind::Metric;
  record.level = bb::Level::Debug;
  record.status = bb::Status::Ok;
  bb::set_record_name(record, "latency.ms");
  record.double_value = 12.5;
  (void)bb::store_value(bb::Value::floating(12.5), record.value);
  return record;
}

std::vector<bb::Record> sample() {
  return {log_record(), error_record(), state_record(), metric_record()};
}

bb::Query must_parse(const std::string& text) {
  std::string error;
  auto query = bb::Query::parse(text, &error);
  CHECK(query.has_value());
  if (!query.has_value()) {
    return bb::Query{};
  }
  CHECK(error.empty());
  return *query;
}

}  // namespace

BB_TEST(query_empty_matches_all) {
  const auto query = must_parse("");
  CHECK(query.match_all());
  for (const auto& record : sample()) {
    CHECK(query.matches(record));
  }
  const auto whitespace = must_parse("   \n\t ");
  CHECK(whitespace.match_all());
}

BB_TEST(query_kind_level_status) {
  const auto records = sample();
  CHECK(must_parse("kind = error").matches(records[1]));
  CHECK(!must_parse("kind = error").matches(records[0]));
  CHECK(must_parse("kind != log").matches(records[1]));
  CHECK(must_parse("level >= warn").matches(records[1]));
  CHECK(must_parse("level >= warn").matches(records[2]));
  CHECK(!must_parse("level >= warn").matches(records[0]));
  CHECK(must_parse("status = timeout").matches(records[1]));
  CHECK(must_parse("status != ok").matches(records[1]));
  CHECK(!must_parse("status != ok").matches(records[3]));
}

BB_TEST(query_string_fields_and_contains) {
  const auto records = sample();
  CHECK(must_parse("message ~ \"TIMEOUT\"").matches(records[0]));
  CHECK(must_parse("message = \"database timeout while waiting\"").matches(records[0]));
  CHECK(must_parse("name = db.query").matches(records[0]));
  CHECK(must_parse("message !~ success").matches(records[0]));
  CHECK(must_parse("state = breaker.state").matches(records[2]));
  CHECK(must_parse("state.new = open").matches(records[2]));
  CHECK(!must_parse("state.new = closed").matches(records[2]));
  CHECK(must_parse("state.old = closed").matches(records[2]));
}

BB_TEST(query_numeric_fields) {
  const auto records = sample();
  CHECK(must_parse("duration > 100ms").matches(records[1]));
  CHECK(!must_parse("duration > 500ms").matches(records[1]));
  CHECK(must_parse("duration >= 250000000").matches(records[1]));
  CHECK(must_parse("span = 7").matches(records[0]));
  CHECK(must_parse("parent = 0").matches(records[0]));
  CHECK(must_parse("correlation = 51966").matches(records[0]));
  CHECK(must_parse("seq >= 20").matches(records[1]));
  CHECK(must_parse("cause = 3").matches(records[0]));
  CHECK(must_parse("value > 10").matches(records[3]));
  CHECK(must_parse("value < 13").matches(records[3]));
}

BB_TEST(query_attributes) {
  const auto records = sample();
  const auto record = records[1];
  CHECK(must_parse("attr.http.status = 500").matches(record));
  CHECK(must_parse("attr[\"db.system\"] = postgres").matches(record));
  CHECK(must_parse("attr.db.system ~ POST").matches(record));
  CHECK(must_parse("attr.retry = true").matches(record));
  CHECK(must_parse("attr.ratio > 0.2").matches(record));
  CHECK(must_parse("attr.ratio <= 0.25").matches(record));
  CHECK(!must_parse("attr.missing = 1").matches(record));
  CHECK(!must_parse("attr.missing != 1").matches(record));
  CHECK(!must_parse("attr.http.status = \"500\"").matches(record));
  CHECK(!must_parse("attr.http.status != \"500\"").matches(record));
  CHECK(!must_parse("attr.retry = false").matches(record));
}

BB_TEST(query_any_text) {
  const auto records = sample();
  CHECK(must_parse("text ~ postgres").matches(records[1]));
  CHECK(must_parse("any ~ timeout").matches(records[0]));
  CHECK(must_parse("any = error").matches(records[1]));
  CHECK(!must_parse("any ~ nothing-here").matches(records[1]));
}

BB_TEST(query_trace_matching) {
  const auto records = sample();
  CHECK(must_parse("trace = 0123456789abcdeffedcba9876543210").matches(records[0]));
  CHECK(!must_parse("trace = 0123456789abcdeffedcba9876543211").matches(records[0]));
  CHECK(must_parse("trace ~ 0123456789abcdef").matches(records[0]));
  CHECK(must_parse("trace !~ ffffffff").matches(records[0]));
}

BB_TEST(query_boolean_precedence_and_not) {
  const auto records = sample();
  CHECK(must_parse("kind = error OR kind = state_change").matches(records[1]));
  CHECK(must_parse("kind = error OR kind = state_change").matches(records[2]));
  CHECK(!must_parse("kind = error OR kind = state_change").matches(records[0]));
  CHECK(must_parse("level = error AND kind = error").matches(records[1]));
  CHECK(must_parse("NOT level = error").matches(records[0]));
  CHECK(!must_parse("NOT level = error").matches(records[1]));
  CHECK(must_parse("(kind = log OR kind = error) AND status != ok")
            .matches(records[1]));
  CHECK(must_parse("kind = log OR kind = error AND level = warn")
            .matches(records[0]));
}

BB_TEST(query_parse_errors_are_explicit) {
  const std::vector<std::string> invalid = {
      "kind =",
      "unknown = 1",
      "kind ~ error",
      "level ~ 2",
      "span = -1",
      "span = 1.5",
      "duration ~ 10ms",
      "(kind = log",
      "kind = log extra",
      "trace = abc",
      "trace > 0123456789abcdeffedcba9876543210",
      "message = \"unterminated",
      "message = \"bad \\q escape\"",
      "duration = 1xs",
      "attr = 1",
      "attr..key = 1",
      "NOT",
      "kind = bogus",
      "((((((((((((((((((((((((((((((((((((kind = log))))))))))))))))))))))))))))))))))))",
  };
  for (const auto& text : invalid) {
    std::string error;
    const auto query = bb::Query::parse(text, &error);
    CHECK(!query.has_value());
    CHECK(!error.empty());
  }
  std::string error;
  CHECK(!bb::Query::parse(std::string(5000, 'a'), &error).has_value());
}

BB_TEST(query_filter_records_and_copy) {
  const auto records = sample();
  const auto query = must_parse("kind = error OR kind = state_change");
  const auto filtered = bb::filter_records(records, query);
  CHECK_EQ(filtered.size(), 2u);
  CHECK_EQ(filtered[0].seq, 20u);
  CHECK_EQ(filtered[1].seq, 30u);
  const auto limited = bb::filter_records(records, query, 1);
  CHECK_EQ(limited.size(), 1u);
  bb::Query copy = query;
  CHECK(copy.matches(records[1]));
  CHECK_EQ(copy.source(), query.source());
}

BB_TEST(query_fuzz_parser_never_crashes) {
  bb::Xoshiro128 random(0x51EED);
  const std::vector<bb::Record> records = sample();
  const std::string alphabet =
      "abcABC0123._-/ :()[]=!<>~\"'\\\n\tAND OR NOT kind level attr seq";
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::size_t length = random.bounded(80);
    std::string text;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
      text.push_back(alphabet[random.bounded(static_cast<std::uint32_t>(alphabet.size()))]);
    }
    std::string error;
    const auto query = bb::Query::parse(text, &error);
    if (query.has_value()) {
      for (const auto& record : records) {
        (void)query->matches(record);
      }
    } else {
      CHECK(!error.empty());
    }
  }
}

BB_TEST(query_boolean_identities_hold) {
  const auto records = sample();
  const std::vector<std::string> texts = {
      "kind = error",
      "level >= warn",
      "message ~ timeout",
      "attr.http.status = 500",
      "state.new = open",
      "duration > 1ms",
  };
  for (const auto& text : texts) {
    const bb::Query query = must_parse(text);
    const bb::Query negated = must_parse("NOT (" + text + ")");
    for (const auto& record : records) {
      CHECK_EQ(query.matches(record), !negated.matches(record));
    }
  }
  const bb::Query left = must_parse("kind = error OR kind = state_change");
  const bb::Query right = must_parse("level >= warn");
  const bb::Query conjunction = must_parse(
      "(kind = error OR kind = state_change) AND level >= warn");
  const bb::Query disjunction = must_parse(
      "(kind = error OR kind = state_change) OR level >= warn");
  for (const auto& record : records) {
    CHECK_EQ(conjunction.matches(record),
             left.matches(record) && right.matches(record));
    CHECK_EQ(disjunction.matches(record),
             left.matches(record) || right.matches(record));
    CHECK_EQ(left.matches(record), left.matches(record));
  }
}

BB_TEST(query_typed_attributes_compare_strictly) {
  bb::Record record = error_record();
  CHECK(must_parse("attr.http.status = 500").matches(record));
  CHECK(must_parse("attr.http.status < 501").matches(record));
  CHECK(must_parse("attr.http.status = 500.0").matches(record));
  CHECK(must_parse("attr.ratio = 0.25").matches(record));
  CHECK(must_parse("attr.retry = true").matches(record));
  CHECK(must_parse("attr.db.system ~ postgres").matches(record));
  CHECK(!must_parse("attr.db.system ~ mysql").matches(record));
  CHECK(!must_parse("attr.retry ~ \"true\"").matches(record));
  record.attrs[0].value = bb::StoredValue{};
  CHECK(!must_parse("attr.http.status != 500").matches(record));
}
