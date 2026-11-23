#include "harness.hpp"
#include "support.hpp"

#include <blackbox/recorder.hpp>
#include <blackbox/util/time.hpp>

#include <array>
#include <limits>
#include <atomic>
#include <string>
#include <filesystem>
#include <thread>
#include <vector>

BB_TEST(recorder_logs_and_chains_causes) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  CHECK(recorder.log(bb::Level::Info, "first"));
  CHECK(recorder.log(bb::Level::Warn, "second",
                     {{"attempt", bb::Value::integer(2)}}));
  const bb::RecorderSnapshot snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 2u);
  CHECK_EQ(snap.first_seq, 1u);
  CHECK_EQ(snap.last_seq, 2u);
  CHECK_EQ(snap.lost, 0u);
  const bb::Record& first = snap.records[0];
  const bb::Record& second = snap.records[1];
  CHECK_EQ(static_cast<int>(first.kind), static_cast<int>(bb::RecordKind::Log));
  CHECK_EQ(bb::record_message(first), std::string_view("first"));
  CHECK_EQ(first.cause_seq, 0u);
  CHECK(first.wall_ns > 0);
  CHECK(second.cause_seq == first.seq);
  CHECK_EQ(static_cast<int>(second.level), static_cast<int>(bb::Level::Warn));
  CHECK_EQ(second.attr_count, 1u);
  CHECK_EQ(bb::stored_key(second.attrs[0]), std::string_view("attempt"));
  CHECK_EQ(bb::owned_value(second.attrs[0].value).as_int(), 2);
}

BB_TEST(recorder_span_lifecycle) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  const bb::SpanHandle handle = recorder.begin_span("request");
  CHECK(handle.valid());
  CHECK(!recorder.current_trace().is_zero());
  CHECK_NE(recorder.current_correlation(), 0u);
  CHECK_EQ(recorder.current_span(), handle.span);
  CHECK_EQ(recorder.current_depth(), 1u);
  CHECK(recorder.log(bb::Level::Info, "inside"));
  CHECK(recorder.end_span(handle, bb::Status::Ok));
  CHECK_EQ(recorder.current_depth(), 0u);
  CHECK_EQ(recorder.current_span(), 0u);

  const bb::RecorderSnapshot snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 3u);
  const bb::Record& begin = snap.records[0];
  const bb::Record& log = snap.records[1];
  const bb::Record& end = snap.records[2];
  CHECK_EQ(static_cast<int>(begin.kind), static_cast<int>(bb::RecordKind::SpanBegin));
  CHECK_EQ(static_cast<int>(end.kind), static_cast<int>(bb::RecordKind::SpanEnd));
  CHECK_EQ(begin.span, handle.span);
  CHECK(begin.trace == handle.trace);
  CHECK_EQ(begin.parent, 0u);
  CHECK_EQ(log.span, handle.span);
  CHECK_EQ(log.cause_seq, begin.seq);
  CHECK_EQ(end.cause_seq, log.seq);
  CHECK(end.duration_ns >= 0);
  CHECK_EQ(static_cast<int>(end.status), static_cast<int>(bb::Status::Ok));
  CHECK_EQ(bb::record_name(end), std::string_view("request"));
  CHECK_EQ(recorder.stats().spans_started, 1u);
  CHECK_EQ(recorder.stats().spans_ended, 1u);
}

BB_TEST(recorder_nested_spans_and_raii) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  bb::SpanHandle outer{};
  bb::SpanHandle inner{};
  {
    bb::Span outer_guard = recorder.span("outer");
    CHECK(outer_guard.active());
    outer = outer_guard.handle();
    {
      bb::Span inner_guard = recorder.span("inner");
      CHECK(inner_guard.active());
      inner = inner_guard.handle();
      CHECK_EQ(recorder.current_span(), inner.span);
      CHECK(recorder.log(bb::Level::Info, "nested log"));
    }
    CHECK_EQ(recorder.current_depth(), 1u);
    CHECK_EQ(recorder.current_span(), outer.span);
  }
  CHECK_EQ(recorder.current_depth(), 0u);
  const bb::RecorderSnapshot snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 5u);
  CHECK_EQ(static_cast<int>(snap.records[0].kind),
           static_cast<int>(bb::RecordKind::SpanBegin));
  CHECK_EQ(static_cast<int>(snap.records[4].kind),
           static_cast<int>(bb::RecordKind::SpanEnd));
  const bb::Record& inner_begin = snap.records[1];
  CHECK_EQ(inner_begin.parent, outer.span);
  CHECK_EQ(inner_begin.span, inner.span);
  const bb::Record& inner_end = snap.records[3];
  CHECK_EQ(inner_end.span, inner.span);
}

BB_TEST(recorder_span_move_transfers_ownership) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  bb::SpanHandle handle{};
  {
    bb::Span first = recorder.span("movable");
    handle = first.handle();
    bb::Span second = std::move(first);
    CHECK(!first.active());
    CHECK(second.active());
  }
  CHECK_EQ(recorder.current_depth(), 0u);
  const auto snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 2u);
  CHECK_EQ(snap.records[1].span, handle.span);
}

BB_TEST(recorder_span_status_is_recorded) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  {
    bb::Span guard = recorder.span("db.query");
    guard.set_status(bb::Status::Timeout);
  }
  const auto snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 2u);
  CHECK_EQ(static_cast<int>(snap.records[1].status),
           static_cast<int>(bb::Status::Timeout));
  CHECK_EQ(static_cast<int>(snap.records[1].level),
           static_cast<int>(bb::Level::Error));
}

BB_TEST(recorder_rejects_mismatched_end) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  const auto outer = recorder.begin_span("outer");
  const auto inner = recorder.begin_span("inner");
  CHECK(!recorder.end_span(outer));
  CHECK_EQ(recorder.stats().spans_dropped, 1u);
  CHECK(recorder.end_span(inner));
  CHECK(recorder.end_span(outer));
  CHECK(!recorder.end_span(inner));
}

BB_TEST(recorder_correlation_and_reset) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  recorder.set_correlation(1234);
  const auto handle = recorder.begin_span("request");
  CHECK_EQ(recorder.current_correlation(), 1234u);
  CHECK(recorder.end_span(handle));
  const auto snap = recorder.snapshot();
  for (const auto& record : snap.records) {
    CHECK_EQ(record.correlation, 1234u);
  }
  recorder.reset_thread_context();
  CHECK_EQ(recorder.current_correlation(), 0u);
  CHECK(recorder.current_trace().is_zero());
  CHECK(recorder.log(bb::Level::Debug, "global"));
  const auto after = recorder.snapshot();
  CHECK_EQ(after.records.back().correlation, 0u);
  CHECK(after.records.back().trace.is_zero());
}

BB_TEST(recorder_state_changes) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  CHECK(recorder.state_change("breaker.state", bb::Value::string("closed"),
                              bb::Value::string("open"), bb::Level::Warn));
  const auto snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 1u);
  const bb::Record& record = snap.records[0];
  CHECK_EQ(static_cast<int>(record.kind),
           static_cast<int>(bb::RecordKind::StateChange));
  CHECK_EQ(bb::record_state_key(record), std::string_view("breaker.state"));
  CHECK_EQ(bb::owned_value(record.state_old).as_string(), std::string("closed"));
  CHECK_EQ(bb::owned_value(record.state_new).as_string(), std::string("open"));
  CHECK_EQ(static_cast<int>(record.level), static_cast<int>(bb::Level::Warn));
  CHECK(!recorder.state_change("bad key", bb::Value::integer(1),
                               bb::Value::integer(2)));
  CHECK_EQ(recorder.stats().rejected, 1u);
}

BB_TEST(recorder_metrics) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  CHECK(recorder.metric("requests", std::int64_t{17}));
  CHECK(recorder.metric("latency.ms", 12.5));
  CHECK(!recorder.metric("bad", std::numeric_limits<double>::infinity()));
  const auto snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 2u);
  CHECK_EQ(snap.records[0].int_value, 17);
  CHECK_EQ(bb::owned_value(snap.records[0].value).as_int(), 17);
  CHECK_EQ(snap.records[1].double_value, 12.5);
  CHECK_EQ(bb::record_name(snap.records[1]), std::string_view("latency.ms"));
  CHECK_EQ(recorder.stats().rejected, 1u);
}

BB_TEST(recorder_rejects_invalid_input) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  CHECK(!recorder.log(bb::Level::Info,
                      std::string(bb::kMaxMessageBytes + 1, 'x')));
  CHECK(!recorder.log(bb::Level::Info, "bad", {{"bad key", bb::Value::integer(1)}}));
  CHECK(!recorder.log(bb::Level::Info, "dupe",
                      {{"a", bb::Value::integer(1)},
                       {"a", bb::Value::integer(2)}}));
  CHECK(!recorder.log(bb::Level::Info, "nine",
                      {{"a", bb::Value::integer(1)},
                       {"b", bb::Value::integer(1)},
                       {"c", bb::Value::integer(1)},
                       {"d", bb::Value::integer(1)},
                       {"e", bb::Value::integer(1)},
                       {"f", bb::Value::integer(1)},
                       {"g", bb::Value::integer(1)},
                       {"h", bb::Value::integer(1)},
                       {"i", bb::Value::integer(1)}}));
  CHECK(!recorder.state_change("k", bb::Value::floating(
                                        std::numeric_limits<double>::quiet_NaN()),
                               bb::Value::null()));
  CHECK_EQ(recorder.snapshot().records.size(), 0u);
  CHECK_EQ(recorder.stats().rejected, 5u);
}

BB_TEST(recorder_span_depth_is_bounded) {
  bb::Recorder recorder(bb::RecorderConfig{512});
  std::array<bb::SpanHandle, 64> handles{};
  for (std::size_t i = 0; i < handles.size(); ++i) {
    handles[i] = recorder.begin_span("level");
    CHECK(handles[i].valid());
  }
  CHECK_EQ(recorder.current_depth(), 64u);
  const auto overflow = recorder.begin_span("too-deep");
  CHECK(!overflow.valid());
  CHECK_EQ(recorder.stats().rejected, 1u);
  for (std::size_t i = handles.size(); i > 0; --i) {
    CHECK(recorder.end_span(handles[i - 1]));
  }
  CHECK_EQ(recorder.current_depth(), 0u);
}

BB_TEST(recorder_snapshot_window_and_loss) {
  bb::Recorder recorder(bb::RecorderConfig{8});
  for (int i = 0; i < 20; ++i) {
    CHECK(recorder.log(bb::Level::Debug, "log " + std::to_string(i)));
  }
  const auto latest = recorder.snapshot(4);
  CHECK_EQ(latest.records.size(), 4u);
  CHECK_EQ(latest.records.front().seq, 17u);
  CHECK_EQ(latest.records.back().seq, 20u);
  CHECK_EQ(latest.lost, 16u);
  const auto all = recorder.snapshot(1000);
  CHECK_EQ(all.records.size(), 8u);
  CHECK_EQ(all.records.front().seq, 13u);
  CHECK_EQ(all.lost, 12u);
  CHECK_EQ(all.ring.produced, 20u);
}

BB_TEST(recorder_threads_have_separate_contexts) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  std::array<bb::TraceId, 2> traces{};
  std::array<bb::CorrelationId, 2> correlations{};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&, t] {
      recorder.reset_thread_context();
      recorder.set_correlation(static_cast<bb::CorrelationId>(100 + t));
      const auto handle = recorder.begin_span("thread." + std::to_string(t));
      if (!handle.valid()) {
        failures.fetch_add(1);
      }
      traces[static_cast<std::size_t>(t)] = recorder.current_trace();
      correlations[static_cast<std::size_t>(t)] = recorder.current_correlation();
      if (!recorder.log(bb::Level::Info, "hello")) {
        failures.fetch_add(1);
      }
      if (!recorder.end_span(handle)) {
        failures.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK_EQ(failures.load(), 0);
  CHECK(traces[0] != traces[1]);
  CHECK_NE(correlations[0], correlations[1]);
  const auto snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 6u);
  int parents_zero = 0;
  for (const auto& record : snap.records) {
    if (record.kind == bb::RecordKind::SpanBegin && record.parent == 0) {
      ++parents_zero;
    }
  }
  CHECK_EQ(parents_zero, 2);
}

BB_TEST(recorder_generic_record_uses_context) {
  bb::Recorder recorder(bb::RecorderConfig{64});
  const auto handle = recorder.begin_span("outer");
  bb::Record record{};
  record.kind = bb::RecordKind::Note;
  record.level = bb::Level::Debug;
  CHECK(bb::set_record_message(record, "generic"));
  CHECK(recorder.record(record));
  CHECK(recorder.end_span(handle));
  const auto snap = recorder.snapshot();
  CHECK_EQ(snap.records.size(), 3u);
  CHECK(snap.records[1].trace == handle.trace);
  CHECK_EQ(snap.records[1].span, handle.span);
  CHECK_EQ(snap.records[1].correlation, snap.records[0].correlation);
}

BB_TEST(recorder_flush_dump_roundtrip_and_listing) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 64;
  config.dump_directory = temp.file("dumps");
  bb::Recorder recorder(config);
  CHECK(recorder.log(bb::Level::Info, "before dump"));
  const auto handle = recorder.begin_span("dumped.span");
  CHECK(handle.valid());
  CHECK(recorder.metric("queue.depth", std::int64_t{3}));
  CHECK(recorder.end_span(handle));
  CHECK(recorder.state_change("breaker.state", bb::Value::string("closed"),
                              bb::Value::string("open")));

  const bb::DumpResult flushed = recorder.flush_dump();
  CHECK(flushed.ok);
  CHECK(!flushed.path.empty());
  CHECK(std::filesystem::exists(flushed.path));

  const auto read = bb::read_dump_file(flushed.path);
  CHECK(read.ok);
  CHECK(read.complete);
  CHECK_EQ(read.records.size(), 5u);
  for (const auto& record : read.records) {
    CHECK(bb::validate_record(record));
  }

  const bb::DumpResult second = recorder.flush_dump();
  CHECK(second.ok);
  CHECK_NE(second.path, flushed.path);
  const auto dumps = recorder.list_dumps(10);
  CHECK_EQ(dumps.size(), 2u);
  CHECK(dumps[0].valid);
  CHECK(dumps[1].valid);

  for (const auto& entry : std::filesystem::directory_iterator(
           std::filesystem::path(config.dump_directory))) {
    CHECK(entry.path().extension() == ".bbx");
  }
}

BB_TEST(recorder_flush_without_directory_fails_cleanly) {
  bb::Recorder recorder(bb::RecorderConfig{16});
  CHECK(recorder.log(bb::Level::Info, "nowhere"));
  const auto result = recorder.flush_dump();
  CHECK(!result.ok);
  CHECK(!result.error.empty());
  CHECK(result.path.empty());
  CHECK(recorder.list_dumps().empty());
}

BB_TEST(recorder_dump_keeps_only_retained_records) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 8;
  config.dump_directory = temp.file("dumps");
  bb::Recorder recorder(config);
  for (int i = 0; i < 20; ++i) {
    CHECK(recorder.log(bb::Level::Debug, "log " + std::to_string(i)));
  }
  const auto result = recorder.flush_dump();
  CHECK(result.ok);
  const auto read = bb::read_dump_file(result.path);
  CHECK(read.ok);
  CHECK_EQ(read.records.size(), 8u);
  CHECK_EQ(read.records.front().seq, 13u);
  CHECK_EQ(read.records.back().seq, 20u);
}

BB_TEST(recorder_dump_retention_by_count_and_size) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 64;
  config.dump_directory = temp.file("count-dumps");
  config.max_dumps = 1;
  bb::Recorder recorder(config);
  CHECK(recorder.log(bb::Level::Info, "first"));
  const auto first = recorder.flush_dump();
  CHECK(first.ok);
  CHECK(recorder.log(bb::Level::Info, "second"));
  const auto second = recorder.flush_dump();
  CHECK(second.ok);
  const auto dumps = recorder.list_dumps(10);
  CHECK_EQ(dumps.size(), 1u);
  CHECK_EQ(dumps[0].name, std::filesystem::path(second.path).filename().string());
  CHECK(std::filesystem::exists(second.path));
  CHECK(!std::filesystem::exists(first.path));

  bb::RecorderConfig bytes_config;
  bytes_config.ring_capacity = 64;
  bytes_config.dump_directory = temp.file("byte-dumps");
  bytes_config.max_dumps = 0;
  bytes_config.max_dump_bytes = 1;
  bb::Recorder bytes_recorder(bytes_config);
  CHECK(bytes_recorder.log(bb::Level::Info, "one"));
  const auto one = bytes_recorder.flush_dump();
  CHECK(one.ok);
  CHECK(bytes_recorder.log(bb::Level::Info, "two"));
  const auto two = bytes_recorder.flush_dump();
  CHECK(two.ok);
  const auto byte_dumps = bytes_recorder.list_dumps(10);
  CHECK_EQ(byte_dumps.size(), 1u);
  CHECK_EQ(byte_dumps[0].name, std::filesystem::path(two.path).filename().string());
}

BB_TEST(recorder_dump_guard_serialises_listing_and_flush) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 32;
  config.dump_directory = temp.file("guard-dumps");
  config.max_dumps = 2;
  bb::Recorder recorder(config);
  CHECK(recorder.log(bb::Level::Info, "guarded"));
  const auto guard = recorder.lock_dumps();
  const auto inside = recorder.list_dumps(10);
  CHECK(inside.empty());
  const auto flushed = recorder.flush_dump();
  CHECK(flushed.ok);
  const auto after = recorder.list_dumps(10);
  CHECK_EQ(after.size(), 1u);
}
