#include "harness.hpp"

#include <blackbox/record.hpp>

#include <cstring>
#include <limits>
#include <string>

namespace {

bb::Record make_valid_record() {
  bb::Record record{};
  record.kind = bb::RecordKind::SpanBegin;
  record.trace = bb::TraceId{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
  record.span = 7;
  record.parent = 0;
  CHECK(bb::set_record_name(record, "db.query"));
  record.wall_ns = 1'700'000'000'000'000'000;
  record.mono_ns = 123;
  return record;
}

}  // namespace

BB_TEST(record_storage_stays_within_budget) {
  CHECK(sizeof(bb::Record) <= bb::kRecordStorageBytes);
  CHECK(sizeof(bb::Record) > 512u);
}

BB_TEST(default_record_is_valid) {
  bb::Record record{};
  std::string error;
  CHECK(bb::validate_record(record, &error));
  CHECK(error.empty());
}

BB_TEST(valid_span_record) {
  bb::Record record = make_valid_record();
  std::string error;
  CHECK(bb::validate_record(record, &error));
  CHECK_EQ(bb::record_name(record), std::string_view("db.query"));
}

BB_TEST(span_record_requires_trace_span_and_name) {
  bb::Record no_trace = make_valid_record();
  no_trace.trace = bb::TraceId{};
  CHECK(!bb::validate_record(no_trace));

  bb::Record no_span = make_valid_record();
  no_span.span = 0;
  CHECK(!bb::validate_record(no_span));

  bb::Record no_name = make_valid_record();
  no_name.name[0] = '\0';
  CHECK(!bb::validate_record(no_name));
}

BB_TEST(state_changes_require_a_key) {
  bb::Record record{};
  record.kind = bb::RecordKind::StateChange;
  CHECK(!bb::validate_record(record));
  CHECK(bb::set_record_state_key(record, "circuit.state"));
  CHECK(bb::validate_record(record));

  bb::Record log{};
  CHECK(bb::set_record_state_key(log, "unexpected"));
  CHECK(!bb::validate_record(log));
}

BB_TEST(record_rejects_out_of_range_enums) {
  bb::Record record{};
  record.kind = static_cast<bb::RecordKind>(99);
  CHECK(!bb::validate_record(record));
  record = bb::Record{};
  record.level = static_cast<bb::Level>(99);
  CHECK(!bb::validate_record(record));
  record = bb::Record{};
  record.status = static_cast<bb::Status>(99);
  CHECK(!bb::validate_record(record));
  record = bb::Record{};
  record.attr_count = bb::kMaxAttributesPerRecord + 1;
  CHECK(!bb::validate_record(record));
}

BB_TEST(record_rejects_negative_duration_and_nonfinite) {
  bb::Record record{};
  record.duration_ns = -1;
  CHECK(!bb::validate_record(record));
  record = bb::Record{};
  record.double_value = std::numeric_limits<double>::infinity();
  CHECK(!bb::validate_record(record));
}

BB_TEST(record_validates_attributes) {
  bb::Record record{};
  record.attr_count = 1;
  bb::store_key("db.query", record.attrs[0]);
  CHECK(bb::store_value(bb::Value::integer(42), record.attrs[0].value));
  CHECK(bb::validate_record(record));

  record.attrs[0].key[0] = ' ';
  CHECK(!bb::validate_record(record));

  bb::Record duplicate{};
  duplicate.attr_count = 2;
  bb::store_key("key", duplicate.attrs[0]);
  bb::store_key("key", duplicate.attrs[1]);
  CHECK(!bb::validate_record(duplicate));

  bb::Record bad_value{};
  bad_value.attr_count = 1;
  bb::store_key("key", bad_value.attrs[0]);
  bad_value.attrs[0].value.type = static_cast<bb::ValueType>(77);
  CHECK(!bb::validate_record(bad_value));
}

BB_TEST(record_rejects_unterminated_fields) {
  bb::Record record{};
  std::memset(record.name, 'x', sizeof(record.name));
  CHECK(!bb::validate_record(record));

  record = bb::Record{};
  std::memset(record.message, 'x', sizeof(record.message));
  CHECK(!bb::validate_record(record));

  record = bb::Record{};
  std::memset(record.state_key, 'x', sizeof(record.state_key));
  CHECK(!bb::validate_record(record));
}

BB_TEST(record_string_setters_never_truncate) {
  bb::Record record{};
  CHECK(bb::set_record_name(record, "ok"));
  CHECK(!bb::set_record_name(
      record, std::string(bb::kMaxNameBytes + 1, 'a')));
  CHECK_EQ(bb::record_name(record), std::string_view("ok"));
  CHECK(!bb::set_record_name(record, std::string_view("a\0b", 3)));

  CHECK(bb::set_record_message(record, "hello"));
  CHECK(!bb::set_record_message(
      record, std::string(bb::kMaxMessageBytes + 1, 'a')));
  CHECK_EQ(bb::record_message(record), std::string_view("hello"));

  CHECK(bb::set_record_state_key(record, "x"));
  CHECK(!bb::set_record_state_key(
      record, std::string(bb::kMaxStateKeyBytes + 1, 'a')));
  CHECK_EQ(bb::record_state_key(record), std::string_view("x"));
}

BB_TEST(record_kind_names_roundtrip) {
  const bb::RecordKind kinds[] = {
      bb::RecordKind::Log,       bb::RecordKind::SpanBegin,
      bb::RecordKind::SpanEnd,   bb::RecordKind::Metric,
      bb::RecordKind::StateChange, bb::RecordKind::Error,
      bb::RecordKind::Mark,      bb::RecordKind::Note,
  };
  for (bb::RecordKind kind : kinds) {
    bb::RecordKind parsed;
    CHECK(bb::record_kind_from_string(bb::to_string(kind), parsed));
    CHECK_EQ(static_cast<int>(parsed), static_cast<int>(kind));
  }
  bb::RecordKind ignored;
  CHECK(!bb::record_kind_from_string("bogus", ignored));

  const bb::Level levels[] = {bb::Level::Trace, bb::Level::Debug,
                              bb::Level::Info, bb::Level::Warn,
                              bb::Level::Error, bb::Level::Fatal};
  for (bb::Level level : levels) {
    bb::Level parsed;
    CHECK(bb::level_from_string(bb::to_string(level), parsed));
    CHECK_EQ(static_cast<int>(parsed), static_cast<int>(level));
  }

  const bb::Status statuses[] = {
      bb::Status::Unset, bb::Status::Ok,        bb::Status::Error,
      bb::Status::Cancelled, bb::Status::Timeout, bb::Status::Rejected};
  for (bb::Status status : statuses) {
    bb::Status parsed;
    CHECK(bb::status_from_string(bb::to_string(status), parsed));
    CHECK_EQ(static_cast<int>(parsed), static_cast<int>(status));
  }
}

BB_TEST(instant_kinds_are_classified) {
  CHECK(bb::kind_is_instant(bb::RecordKind::Log));
  CHECK(bb::kind_is_instant(bb::RecordKind::Metric));
  CHECK(!bb::kind_is_instant(bb::RecordKind::SpanBegin));
  CHECK(!bb::kind_is_instant(bb::RecordKind::SpanEnd));
  CHECK(!bb::kind_is_instant(bb::RecordKind::StateChange));
}
