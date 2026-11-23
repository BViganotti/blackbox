# Architecture

Blackbox is a static C++20 library plus three thin executables:

```text
libblackbox.a
├── core/   record, ring, recorder, codec, dump, crash, causal, query, json
├── ui/     loopback HTTP/SSE server, JSON presentation
└── demo/   synthetic service and launcher application

blackbox_launcher   demo + UI + browser
blackbox_demo       the same application, provided as an example binary
blackbox_bench      measured benchmarks
blackbox_fuzz       deterministic fuzz harness
blackbox_tests      unit/property/malformed/fork tests
```

The UI assets live in `ui/` and are converted to a generated C++ translation
unit at build time (`cmake/embed_assets.cmake`). The final binaries do not read
HTML, CSS or JS from disk.

## Data flow

```text
caller thread
  │  log/error/metric/state_change/span
  ▼
Recorder ── thread-local trace, current span, correlation, last seq
  │  Record (fixed 2048-byte budget, no heap storage)
  ▼
Ring ── 4096 slots × 2048 bytes, seqlock, atomic payload words
  │
  ├── snapshot() ──▶ CausalGraph ──▶ layout / waterfall / explain / compare
  │                        │
  │                        ▼
  │                    JSON ──▶ HTTP/SSE ──▶ browser
  │
  ├── flush_dump() ──▶ atomic .bbx
  └── crash signal ──▶ preallocated .bbx
```

## Record model

`bb::Record` is one trivially copyable struct covering every observation:
logs, errors, metrics, span starts/ends, state changes and marks. It stores
names, messages and attribute values in fixed inline arrays. There are no
pointers, variants with heap state, or lifetime edges, so a record can be
`memcpy`'d into a ring slot, read under a seqlock and written to a dump without
special cases.

`validate_record` is the trust boundary. It checks enum ranges, NUL termination,
attribute key syntax, duplicate keys, finite values, span requirements and state
invariants. The ring, dump writer, dump reader and causal builder all call it.
Invalid records are counted and rejected; they are never silently repaired.

## Ring

The ring is multi-producer/single-consumer-friendly but supports any number of
readers:

- Producers serialise sequence assignment with a small yielded spin lock.
- A producer writes a slot's odd seqlock stamp, stores payload words as relaxed
  atomics, then publishes an even stamp with release ordering.
- Readers load the stamp with acquire, copy the atomic words, and re-check the
  stamp. A mismatch or odd stamp discards the copy.
- Because payload access is through `std::atomic<uint64_t>`, there is no C++
  data race; TSan is clean. A record is never returned torn.
- Producers never wait for readers. When full, the oldest slot is overwritten.
  Readers compute how many sequences they missed and report `lost`.

The sequence is 1-based; zero means "not stored". `for_each_published_signal`
walks published slots without locks or allocation and is used by the crash path.

## Recorder and thread-local context

Each thread has a `Recorder::ThreadState` keyed by an instance token:

- current `TraceId`, `SpanId`, `CorrelationId`,
- the last assigned sequence,
- a fixed 64-frame span stack with start time and name.

Every emitted record gets `cause_seq = last_sequence`, then becomes the new last
sequence. Spans add parent links and a begin→end lifecycle edge. Correlation ids
group work across threads. No allocation is needed on the recording path.

RAII `Span` objects end themselves with their last status. Explicit
`begin_span`/`end_span` is available for generated bindings. The span stack depth
is bounded; exceeding it rejects the new span.

## Dump and crash

The dump format is versioned and checksummed. Normal writes are atomic
(temp → fsync → rename). The reader validates size, magic, versions, flags,
frame CRCs, footer counts and record ordering. Recovery mode returns the valid
prefix and flags the damage.

The crash handler is installed for five fatal signals, uses a 256 KiB alternate
stack and a static encode buffer, and performs only async-signal-safe work:
atomic reads, `open`, `write`, `lseek`, `fsync`, `close`, `rename`, `unlink`,
`raise`, `_exit`. It re-raises the original signal so the process still dies
with the expected status. See [crash-safety.md](crash-safety.md).

## Causal analysis

`build_causal_graph` sorts records by sequence and validates them. Edges are
forward-only by construction:

- `Cause` from `cause_seq`,
- `Parent` from a span's parent id to the parent span begin,
- `Span` from a span begin to its end,
- `Correlation` between consecutive records with the same correlation id.

Unresolved parent ids are counted, not invented. Since every edge increases the
sequence, the graph is acyclic without a separate cycle check.

`layout_causal_graph` assigns the longest-path layer and deterministic rows.
`span_waterfall` joins begin/end records and computes nesting depth.
`explain_failure` walks backwards with priority Cause → Span → Parent →
Correlation, then picks the earliest timeout, error, failed span or abnormal
state change as the root cause. `state_timeline` groups deltas in first-seen
order. `compare_traces` reports the first event-shape or state divergence.

## Query

The lexer and recursive-descent parser enforce source, token, node and depth
limits. Query values are typed (string, int, float, bool, ordinal) and compared
type-aware. The AST is immutable and shared by copies. Evaluation allocates
nothing and never scans outside the record.

## HTTP/SSE server

- Binds `AF_INET`, `INADDR_LOOPBACK`, port 0 (ephemeral), never a public
  interface.
- Generates a 128-bit random token. Requests are authorised by token query,
  cookie, `Authorization: Bearer` or `X-Blackbox-Token`; mutation endpoints
  require the header.
- Fixed accept thread and 12 fixed workers with a bounded queue. Request
  headers, target, parameter count, parameter size and bodies are bounded.
- One request per connection. SSE connections are capped at four and share the
  worker pool; they send a snapshot event every two seconds and a keepalive
  comment.
- Snapshots filter by time and query, build the causal analysis and serialise
  through a bounded JSON writer (8 MiB). Oversized responses fail with an
  explicit 500 rather than being truncated.
- Dump listing, download and open are restricted to `.bbx` files named in the
  configured dump directory; paths never come directly from the request.

## Launcher and demo

`run_demo_app` creates the recorder, demo service and server, prints the token
URL, optionally opens the system browser through `/usr/bin/open` (no shell), and
waits for SIGINT/SIGTERM or `--run-seconds`. The demo service is a deterministic
request → timeout → retry → breaker → fallback → recovery loop on its own thread.
It flushes a dump every twelve scenarios, exercising the retention path.

## Failure behaviour

- Allocation failure before recording is allowed to propagate; after startup the
  recording path does not allocate.
- Invalid input is rejected with a counter and an explicit error string.
- Malformed dumps and queries fail cleanly; the fuzz harness asserts invariants.
- The server never binds beyond loopback and never exposes file paths from
  request strings.
- A crash dump may be lost if the disk is full, the directory disappears or the
  process is killed with an uncatchable signal; LIMITATIONS.md states this.
