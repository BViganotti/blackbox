# Embedding Blackbox

The library is header-first: link `libblackbox.a` and include
`<blackbox/recorder.hpp>`. There is no global initialisation and no required
network access. A process can use the recorder without the UI at all.

## Minimal recorder

```cpp
#include <blackbox/recorder.hpp>

bb::RecorderConfig config;
config.ring_capacity = 4096;                       // 16 MiB
config.dump_directory = "/var/tmp/my-service";     // optional
bb::Recorder recorder(config);

recorder.log(bb::Level::Info, "started",
             {{"service", bb::Value::string("orders")}});
```

`ring_capacity` is rounded up to a power of two and must be in
`[Ring::kMinCapacity, Ring::kMaxCapacity]`. Construction throws
`std::invalid_argument` for invalid capacity and `std::bad_alloc` if the ring
cannot be allocated. After construction the recording path does not allocate.

## Requests, spans and correlation

```cpp
recorder.reset_thread_context();                   // start a request
recorder.set_correlation(bb::new_correlation_id());

{
  bb::Span request = recorder.span(
      "http.request",
      {{"http.route", bb::Value::string("/orders")},
       {"http.method", bb::Value::string("GET")}});

  {
    bb::Span query = recorder.span("db.query",
        {{"db.system", bb::Value::string("postgres")}});
    // ... do work ...
    if (failed) {
      recorder.error("database timeout", bb::Status::Timeout,
                     {{"db.timeout_ms", bb::Value::integer(120)}});
      query.set_status(bb::Status::Timeout);
    } else {
      query.set_status(bb::Status::Ok);
    }
  }

  request.set_status(bb::Status::Ok);
}
```

The outer span's destructor ends it with its last status. Logs, errors, metrics
and state changes inside the span inherit the current trace, span and
correlation. Every record also carries the sequence of the previous record on
the same thread as `cause_seq`.

`begin_span`/`end_span` are available when RAII is inconvenient. Handles are
only valid for the thread and nesting position that created them. Nesting is
bounded to 64 levels; exceeding it returns an invalid handle and increments the
rejected counter.

## Other record kinds

```cpp
recorder.note("cache warmed");
recorder.mark("deploy.start");
recorder.metric("queue.depth", std::int64_t{17});
recorder.metric("latency.ms", 12.5);
recorder.state_change("breaker.state",
                      bb::Value::string("closed"),
                      bb::Value::string("open"),
                      bb::Level::Warn);
```

`state_change` keys follow the same rules as attribute keys. Values are owned
and bounded; an invalid or too-large value returns `false` without recording.
`metric(double)` rejects NaN and infinity.

## Custom records

```cpp
bb::Record record{};
record.kind = bb::RecordKind::Note;
record.level = bb::Level::Debug;
record.int_value = 42;
bb::set_record_message(record, "custom observation");
bool stored = recorder.record(record);  // fills zero context fields
```

`record()` fills a zero trace/span/correlation from the thread context, assigns
timestamps when zero, validates and pushes. It does not manipulate the span
stack.

## Reading and analysis

```cpp
bb::RecorderSnapshot snapshot = recorder.snapshot(1200);
for (const bb::Record& record : snapshot.records) {
  // ascending sequence order
}

std::string error;
bb::CausalGraph graph = bb::build_causal_graph(snapshot.records, &error);
bb::GraphLayout layout = bb::layout_causal_graph(graph);
bb::FailureExplanation failure = bb::explain_latest_failure(graph);
std::vector<bb::SpanRow> spans = bb::span_waterfall(graph);
std::vector<bb::StateHistory> states = bb::state_timeline(graph);
```

The snapshot is a copy; it remains valid after the ring overwrites those slots.
`CausalGraph` owns its own copy of the records.

## Dumps

```cpp
bb::DumpResult dump = recorder.flush_dump();
if (!dump.ok) {
  // dump.error
}

bb::DumpReadOptions options;
options.require_footer = false;       // recovery mode
bb::DumpReadResult read = bb::read_dump_file(dump.path, options);
for (const bb::Record& record : read.records) {
  // ...
}
if (!read.complete) {
  // read.truncated / read.corrupted / read.error
}
```

`flush_dump()` requires `config.dump_directory`. Writes are atomic and prune
oldest dumps beyond `config.max_dumps`. The reader is bounded and validates
every returned record.

## Query

```cpp
auto query = bb::Query::parse("level >= warn AND message ~ timeout");
if (query) {
  auto matching = bb::filter_records(snapshot.records, *query, 1000);
}
```

See [query.md](query.md).

## UI server

```cpp
#include <blackbox/ui_server.hpp>

bb::UiServer server(recorder);
std::string error;
if (!server.start(&error)) {
  // ...
}
std::printf("%s\n", server.url().c_str());
bb::open_browser(server.url());
// ... keep the process alive ...
server.stop();
```

`UiServer` binds only `127.0.0.1` and requires its random token. A demo trigger
callback is optional:

```cpp
bb::UiServer server(recorder, [] { /* enqueue demo work */ });
```

The server can be stopped and started again; `Recorder` must outlive it.

## Thread-safety summary

| Object / call | Rule |
|---|---|
| `Recorder::log/error/metric/state_change/record/span` | safe from any thread |
| Thread-local context and `Span` handles | per-thread; do not share handles |
| `Recorder::snapshot` | safe concurrently with recording |
| `Recorder::flush_dump` | serialised internally; safe concurrently |
| `Recorder::list_dumps` | safe, may race with file creation |
| `CausalGraph`, query, dump reader | immutable/locally owned after construction |
| `UiServer::start/stop` | call from one thread; request handling is concurrent |
| `Recorder` destruction | only when no other thread is recording or holding spans |
| `Span` destruction | must happen before its Recorder is destroyed |
| Crash handler | one per process; see [crash-safety.md](crash-safety.md) |

## Errors and limits

Recording APIs return `false` rather than throwing for invalid input. Inspect
`Recorder::stats()` for rejected records and `RingStats::lost`/`overwritten`
for retention pressure. Detailed limits are in [LIMITATIONS.md](../LIMITATIONS.md)
and the README table.
