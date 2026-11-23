#include <blackbox/present.hpp>

#include <blackbox/util/time.hpp>

#include <string>

namespace bb {

void write_value_json(JsonWriter& out, const Value& value) {
  switch (value.type()) {
    case ValueType::Null:
      out.null();
      break;
    case ValueType::Bool:
      out.boolean(value.as_bool());
      break;
    case ValueType::Int:
      out.number(value.as_int());
      break;
    case ValueType::Float:
      out.number(value.as_double());
      break;
    case ValueType::String:
      out.string(value.as_string());
      break;
  }
}

void write_stored_value_json(JsonWriter& out, const StoredValue& value) {
  switch (value.type) {
    case ValueType::Null:
      out.null();
      break;
    case ValueType::Bool:
      out.boolean(value.boolean);
      break;
    case ValueType::Int:
      out.number(value.integer);
      break;
    case ValueType::Float:
      out.number(value.real);
      break;
    case ValueType::String:
      out.string(stored_text(value));
      break;
  }
}

void write_record_json(JsonWriter& out, const Record& record) {
  out.begin_object();
  out.key("seq");
  out.number(record.seq);
  out.key("wall_ns");
  out.number(record.wall_ns);
  out.key("time");
  if (record.wall_ns > 0) {
    out.string(format_wall_ns(record.wall_ns));
  } else {
    out.null();
  }
  out.key("mono_ns");
  out.number(record.mono_ns);
  out.key("kind");
  out.string(to_string(record.kind));
  out.key("level");
  out.string(to_string(record.level));
  out.key("status");
  out.string(to_string(record.status));
  out.key("trace");
  out.string(record.trace.to_hex());
  out.key("span");
  out.number(record.span);
  out.key("parent");
  out.number(record.parent);
  out.key("correlation");
  out.number(record.correlation);
  out.key("cause_seq");
  out.number(record.cause_seq);
  out.key("duration_ns");
  out.number(record.duration_ns);
  out.key("name");
  out.string(record_name(record));
  out.key("message");
  out.string(record_message(record));
  out.key("state_key");
  out.string(record_state_key(record));
  out.key("int_value");
  out.number(record.int_value);
  out.key("double_value");
  out.number(record.double_value);
  out.key("value");
  write_stored_value_json(out, record.value);
  out.key("state_old");
  write_stored_value_json(out, record.state_old);
  out.key("state_new");
  write_stored_value_json(out, record.state_new);
  out.key("attrs");
  out.begin_object();
  for (std::size_t i = 0; i < record.attr_count; ++i) {
    out.key(stored_key(record.attrs[i]));
    write_stored_value_json(out, record.attrs[i].value);
  }
  out.end_object();
  out.end_object();
}

void write_ring_stats_json(JsonWriter& out, const RingStats& stats) {
  out.begin_object();
  out.key("capacity");
  out.number(stats.capacity);
  out.key("memory_bytes");
  out.number(stats.memory_bytes);
  out.key("produced");
  out.number(stats.produced);
  out.key("rejected");
  out.number(stats.rejected);
  out.key("overwritten");
  out.number(stats.overwritten);
  out.key("lost");
  out.number(stats.lost);
  out.key("oldest_seq");
  out.number(stats.oldest_seq);
  out.key("newest_seq");
  out.number(stats.newest_seq);
  out.end_object();
}

void write_recorder_stats_json(JsonWriter& out, const RecorderStats& stats) {
  out.begin_object();
  out.key("ring");
  write_ring_stats_json(out, stats.ring);
  out.key("rejected");
  out.number(stats.rejected);
  out.key("spans_started");
  out.number(stats.spans_started);
  out.key("spans_ended");
  out.number(stats.spans_ended);
  out.key("spans_dropped");
  out.number(stats.spans_dropped);
  out.end_object();
}

}  // namespace bb
