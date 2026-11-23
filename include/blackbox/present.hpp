#pragma once

#include <blackbox/attribute.hpp>
#include <blackbox/json.hpp>
#include <blackbox/record.hpp>
#include <blackbox/recorder.hpp>

namespace bb {

/// Writes an owned Value as a typed JSON value (null, bool, number, string).
void write_value_json(JsonWriter& out, const Value& value);

/// Writes a fixed stored value with the same representation as write_value_json.
void write_stored_value_json(JsonWriter& out, const StoredValue& value);

/// Writes one record as a self-describing JSON object. All strings are escaped;
/// timestamps appear both as nanoseconds and as UTC ISO-8601.
void write_record_json(JsonWriter& out, const Record& record);

void write_ring_stats_json(JsonWriter& out, const RingStats& stats);
void write_recorder_stats_json(JsonWriter& out, const RecorderStats& stats);

}  // namespace bb
