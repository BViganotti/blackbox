# Blackbox

**An always-on causal flight recorder for local C++ services.**

Blackbox keeps a bounded rolling history of structured events, spans, logs,
metrics, state deltas and correlation IDs. When a request goes wrong you open a
local workbench and see the causal chain — request → retry → timeout → circuit
breaker → fallback — instead of reading a wall of log lines.

It is a desktop-style developer workbench served from the binary, not a CLI and
not a hosted service. The C++ core is also an embeddable library.

```text
                 ┌─────────────── local process ───────────────┐
 service code ──▶│ Recorder → bounded MPSC ring → dump/crash   │
                 │      │             │                        │
                 │      │             └─▶ portable .bbx bundle  │
                 │      ▼                                      │
                 │ causal analysis → 127.0.0.1 HTTP/SSE → browser UI
                 └─────────────────────────────────────────────┘
```

## What it does

- Records logs, errors, metrics, span begin/end, state transitions and notes in
  one fixed-size record type.
- Links records automatically with a per-thread `cause_seq`, span parent links
  and correlation IDs.
- Keeps a bounded ring (default 4096 records, 16 MiB) and never grows without a
  limit. Overwritten history is reported, not hidden.
- Reconstructs a DAG, deterministic layered layout, request waterfall, state
  timeline and an explain-failure walk from an error back to its root cause.
- Compares two traces and reports the first event or state divergence.
- Writes portable, checksummed `.bbx` dumps atomically.
- Installs an async-signal-safe crash handler that writes a complete dump on
  `SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGILL` or `SIGFPE` before re-raising.
- Serves an original HTML/CSS/JS workbench from the binary on a random loopback
  port with a random session token and server-sent events.
- Has no runtime dependencies beyond the C++ standard library, POSIX and macOS
  system APIs. No internet, CDN, external fonts, vendored code or CI.

## Requirements

- C++20 compiler (Apple Clang 15+ or recent Clang/GCC)
- CMake 3.20+
- Ninja
- POSIX platform; macOS is the primary target

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Warnings are errors. Sanitizer builds:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=address
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
./build-asan/blackbox_fuzz --iterations 5000

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

`-DBLACKBOX_SANITIZE=address` enables ASan and UBSan together.

Fuzz and benchmark:

```sh
./build/blackbox_fuzz --iterations 30000 --seed 12345
./build/blackbox_bench            # full
./build/blackbox_bench --quick
```

## Run the workbench

```sh
./build/blackbox_launcher
```

The launcher starts the demo service, binds `127.0.0.1` on an ephemeral port,
opens the system browser at a URL containing a random token, and keeps running
until interrupted. It never asks for terminal input.

Useful flags:

```text
--no-browser          do not launch the browser (the URL is still printed)
--run-seconds N       stop after N seconds; 0 means until SIGINT/SIGTERM
--dump-dir PATH       dump directory (default: platform data directory)
--open-dump PATH      open a .bbx bundle on launch
--no-demo             do not generate demo traffic
--capacity N          ring capacity, 8..131072 records
```

The demo creates a deterministic request lifecycle every 1.2 s:

1. `http.request` starts.
2. Two `db.query` attempts time out, each with an error record.
3. `breaker.state` changes from `closed` to `open`.
4. `fallback.cache` serves a cached response.
5. A later request runs with the breaker open; after two fallbacks it closes
   and a normal database query succeeds.

The **Failure** panel shows the causal chain and the earliest root cause.
The **Demo** button triggers one extra scenario through `POST /api/demo/trigger`.

## Workbench

- **Causal graph** — layered canvas DAG with cause, parent-span, span-life and
  correlation edges. Defaults to the latest failure chain; *Scope* shows all
  retained nodes. Drag to pan, wheel to zoom, double-click to fit.
- **Timeline** — span bars plus error, state and log markers on a shared time
  ruler. Click a marker to inspect it.
- **Request waterfall** — nested span rows with relative start, duration and
  status. Click a row to jump to the span begin record.
- **Failure inspector** — summary, clickable chain, findings and root cause.
- **Event inspector** — fields, attributes, state values, cause chain and
  navigation to the parent span.
- **State inspector** — every state transition grouped by key, with old/new
  values and links back to the record.
- **Dumps** — list, open offline, download or take a dump now.
- **Query** — a bounded predicate language over all fields.

Keyboard: `/` focuses the query, `R` refreshes, `D` dumps, `Esc` clears the
selection.

## Query language

```text
kind = error AND duration > 100ms
level >= warn
attr.http.status = 500
message ~ timeout
state.new = open
trace ~ 0123456789abcdef
NOT kind = metric
```

Fields: `kind`, `level`, `status`, `name`, `message`, `text`/`any`, `trace`,
`span`, `parent`, `correlation`, `seq`, `cause`, `duration`, `state`,
`state.old`, `state.new`, `value`, `attr.KEY`, `attr["KEY"]`.

Operators: `=`, `!=`, `<`, `<=`, `>`, `>=`, `~` (ASCII case-insensitive
contains), `!~`, `AND`, `OR`, `NOT`, parentheses. Integers are nanoseconds for
`duration`; suffixes `ns`, `us`, `ms`, `s` are accepted. See
[docs/query.md](docs/query.md) for the full grammar and limits.

## Embedding

```cpp
#include <blackbox/recorder.hpp>

int main() {
  bb::RecorderConfig config;
  config.ring_capacity = 4096;
  config.dump_directory = "/tmp/my-service-dumps";

  bb::Recorder recorder(config);
  recorder.set_correlation(bb::new_correlation_id());

  {
    bb::Span request = recorder.span("http.request",
        {{"http.route", bb::Value::string("/orders")}});
    {
      bb::Span query = recorder.span("db.query");
      recorder.error("database timed out", bb::Status::Timeout);
      query.set_status(bb::Status::Timeout);
    }
    recorder.state_change("breaker.state", bb::Value::string("closed"),
                          bb::Value::string("open"));
    request.set_status(bb::Status::Ok);
  }

  bb::RecorderSnapshot snapshot = recorder.snapshot();
  bb::DumpResult dump = recorder.flush_dump();
}
```

Records are self-contained and trivially copyable; spans are movable RAII
guards. Attribute keys and text are bounded and rejected when too long, never
silently truncated. See [docs/embedding.md](docs/embedding.md).

## Dumps and crash safety

A dump is a portable file with a magic header, string table, checksummed record
frames and a footer with a global record CRC. Writes go to a temporary file,
`fsync`, then atomic `rename`. Readers support strict mode (any damage fails) and
recovery mode (returns the valid prefix and reports `truncated`/`corrupted`).
See [docs/dump-format.md](docs/dump-format.md).

The crash handler uses only preallocated storage, atomic reads, raw `write`,
`fsync` and `rename`; it never allocates, locks or uses stdio. It runs on an
alternate signal stack and re-raises the signal afterwards.

## Bounded resources

| Resource | Bound |
|---|---:|
| Ring records | 8..131072, default 4096 |
| One record | 2048 bytes |
| Attributes per record | 8 |
| Attribute key / text | 27 / 96 bytes |
| Name / message / state key | 63 / 127 / 27 bytes |
| Span nesting | 64 levels per thread |
| Dump file read | 256 MiB, 1,000,000 records, 8 MiB/frame |
| Query source / tokens / nodes / depth | 4096 / 1024 / 512 / 32 |
| HTTP headers / params / body | 16 KiB / 32 params / 4 KiB |
| SSE streams | 4 |
| Retained dumps | 64 per directory |

See [LIMITATIONS.md](LIMITATIONS.md) for what is intentionally not supported.

## Benchmarks

Measured with the full suite on an Apple M1 Max (macOS 26.7.3, arm64, release
build). These are host measurements, not guarantees:

| Benchmark | Result |
|---|---:|
| Ring single-producer push | ~237 ns/event (4.2M events/s) |
| Ring four-producer push | ~562 ns/event (1.8M events/s) |
| Ring snapshot read | ~149 ns/record (6.7M records/s) |
| Dump write (4096 records) | ~14.8 MiB/s |
| Dump strict read + validate | ~101 MiB/s (677 ns/record) |
| Query evaluation | ~9.3 ns/record |
| Causal build + layout + explain (5000 records) | ~1.44 ms/trace |
| Record JSON serialisation | ~181 MiB/s |

Run `./build/blackbox_bench` to reproduce on your machine.

## Repository layout

```text
include/blackbox/   public embeddable API
src/core/           ring, recorder, codec, dump, crash, causal, query, JSON
src/ui/             HTTP/SSE server and presentation
src/demo/           demo service and launcher app
src/app/            launcher, bench and fuzz entry points
ui/                 original HTML/CSS/JS, compiled into the binary
examples/           embeddable demo entry point
tests/              unit, property, malformed-input and fork/crash tests
docs/               format, architecture, query, embedding and hardening docs
```

## Design notes

- One fixed `Record` type instead of a variant tree keeps the ring contiguous
  and allocation-free after startup.
- Ring payloads are atomic words protected by a seqlock. Readers can never see a
  torn record, and TSan sees no data races.
- Producers never wait for readers. Old history is overwritten and readers are
  told how much they missed.
- All parsing paths are bounded and fail explicitly. The dump reader, query
  parser and HTTP parser are fuzzed or malformed-input tested.
- Soundness beats throughput: modest performance is acceptable; UB, unbounded
  memory and silent truncation are not.

## Status

Version 1.0.0. See [CHANGELOG.md](CHANGELOG.md) and
[FINAL_REPORT.md](FINAL_REPORT.md) for the verification record.

## License

MIT. See [LICENSE](LICENSE).
