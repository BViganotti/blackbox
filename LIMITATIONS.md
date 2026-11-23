# Limitations

Blackbox is deliberately a local, single-machine workbench. The following are
known and intentional boundaries, not hidden failures.

## Platform

- macOS is the primary target and the only platform manually exercised. The
  code uses POSIX sockets, signals, `fork`, `sigaltstack`, `fsync` and
  `/dev/urandom`/`getentropy`; other POSIX systems should build but have not
  been verified.
- Windows is not supported.
- The browser launcher uses `/usr/bin/open` on macOS and `xdg-open` elsewhere.
  On a headless machine it fails gracefully and prints the URL.
- No CI configuration exists by design. Verification is local and recorded in
  `FINAL_REPORT.md`.

## UI and transport

- Plain HTTP on loopback only, with a bearer/cookie token. There is no TLS,
  multi-user authentication, remote access or reverse-proxy mode.
- One request per TCP connection. There is no keep-alive for ordinary requests.
- Server-sent events send a full filtered snapshot every two seconds rather than
  a compact delta stream. This favours simple correctness over bandwidth.
- Four concurrent SSE clients and twelve workers. Additional connections receive
  429/503 rather than unbounded thread growth.
- Snapshot responses are capped at 8 MiB and 4000 records. If a request would
  exceed the JSON budget it fails with an explicit error; it is not truncated.
- Dump downloads are capped at 64 MiB even though the reader accepts 256 MiB
  files. Use `read_dump_file` directly for larger bundles.
- The UI has no virtualised event list; it renders at most 1200 rows per
  snapshot. Older records remain available through time windows and queries.
- There is no embedded browser: the system browser is used.

## Recording

- One record is at most 2048 bytes, with 8 attributes, 63-byte names,
  127-byte messages, 27-byte keys/state keys and 96-byte text values. Oversized
  input is rejected, never truncated.
- Span nesting is limited to 64 levels per thread.
- Ring capacity is at most 131072 records (256 MiB) and at least 8.
- Context is thread-local. A span begun on one thread must be ended on that
  thread, and `Span` handles cannot be shared across threads.
- A `Span` or `Recorder` must not be destroyed while another thread is using it.
  Destruction is not synchronized with active producers or readers.
- The ring overwrites the oldest record when full. Consumers are told how many
  records they missed; the recorder does not block.
- There is no sampling, rate limiting or per-key cardinality control.
- Instrumentation is manual; there are no automatic hooks into HTTP frameworks
  or database drivers.

## Dumps

- No compression. A full 8192-record ring dump is roughly tens of MiB depending
  on strings.
- No encryption. Dumps may contain request metadata and error text; treat them as
  sensitive local files (mode 0600).
- The string table is bounded at 16384 entries and 256 bytes per string. Dumps
  with more unique strings fail cleanly.
- Dumps are not merged or appended. Each flush writes a complete retained window.
- Retention deletes oldest `.bbx` files beyond `max_dumps` (default 64) on a
  successful flush; crash dumps in the same directory are included in that
  listing.
- A truncated dump is opened in recovery mode by the UI, but strict tooling
  rejects it.
- The format has no migration path yet. Version changes require a new reader.

## Crash handling

- `SIGKILL` and kernel failures cannot be captured.
- Only one handler exists per process. Embedders with an existing handler should
  disable installation.
- The handler may fail if the disk is full, the directory was removed or the
  signal arrives while the alternate stack is unusable.
- `fsync` is best effort; a power loss immediately after the crash may lose the
  file even though the process died.
- Records being written at the instant of the signal are skipped rather than
  included torn.
- Crash dumps are not pruned while the dying process runs.

## Analysis

- Parent span ids that cannot be resolved are counted, not fabricated.
- Explain-failure picks the earliest error/timeout/failed span/abnormal state in
  the causal chain; a heuristic, not a proof of causality.
- Divergence comparison aligns records by position, not by semantic key. Two
  traces with inserted records diverge at the insertion, which is intended but
  not a full trace alignment algorithm.
- The graph becomes visually dense with thousands of nodes; the default
  failure-chain scope mitigates this but the UI has no minimap or grouping.
- Query language has no regex, aggregation, arithmetic or joins.
- Metrics are point values only; there is no rolling aggregation engine.

## Build and packaging

- There is no install target or package manifest yet; the build produces
  binaries in the build directory.
- The asset embedder requires CMake but no Python or external tool.
- No third-party libraries, vendored code, CDN assets or external fonts are
  used, by design.
