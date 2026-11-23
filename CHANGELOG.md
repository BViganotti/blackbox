# Changelog

All notable changes are documented here. The format follows Keep a Changelog
and the project uses semantic versioning.

## [1.0.0] — 2026-11-23

### Added

- Fixed-size bounded `Record` model for logs, spans, metrics, state changes and
  errors, with strict validation.
- Link-free multi-producer ring with seqlock readers, overwrite accounting and
  signal-safe iteration.
- Embeddable `Recorder` with thread-local trace/span/correlation context,
  causal chains, RAII spans and atomic statistics.
- Portable `.bbx` dump format: versioned header, bounded string table,
  checksummed frames, footer CRC and atomic file replacement.
- Strict and recovery dump readers with truncation/corruption reporting.
- Async-signal-safe crash handler for SIGSEGV, SIGABRT, SIGBUS, SIGILL and
  SIGFPE, using preallocated storage and an alternate signal stack.
- Causal graph construction with cause, parent-span, span-life and correlation
  edges; deterministic layered layout.
- Explain-failure walk from an error to the earliest root cause, span
  waterfall, state timelines and trace divergence comparison.
- Bounded query language with typed comparisons, attributes, free-text search,
  duration units and hard parser limits.
- Original local workbench: canvas causal graph with failure-chain focus,
  timeline, event list, request waterfall, failure/event/state/dump/query
  inspectors, SSE updates, query bar and dump browser.
- Loopback HTTP server with random token authentication, bounded request
  parsing, fixed worker pool, snapshot/query/stats/dump/compare/SSE endpoints.
- Deterministic demo service: request → two DB timeouts → breaker open →
  cached fallback → open-breaker request → recovery, with automatic dumps.
- Launcher that prints a token URL and opens the system browser; `--open-dump`
  for offline bundles, `--no-demo` for inspection, `--max-dumps` retention and
  `--version`/`--help`.
- Unit, property, malformed-input, fork/crash and real-socket server tests.
- Deterministic fuzz harness for dumps, queries, record codec and varints.
- Benchmarks for ring, dump, query, causal analysis and JSON.
- Dump comparison endpoint and Compare tab: first event-shape and state
  divergence with sequence links.
- Local query history (twelve entries), quick filters, timeline tooltips,
  event JSON copy and periodic dump-list refresh.
- CMake install rules, headless embedding example and launcher/embedding smoke
  tests.
- Documentation: architecture, dump format, query language, embedding, crash
  safety, UI, design decisions, performance, testing and security; MIT license.

### Fixed and hardened

- Dump writer and reader now share a 256 MiB size contract, with a per-write
  override; oversized writes fail without publishing a final file.
- Dump metadata distinguishes header validity from footer completeness, and the
  dump browser labels complete vs header-only bundles.
- Dump retention supports count, total bytes and optional age; the just-written
  dump is always kept.
- A recursive dump-directory guard prevents retention from unlinking a bundle
  during list/download/open/compare.
- Crash encoding no longer relies on `memcpy`; supported targets assert lock-free
  64-bit atomics. Spans survive early Recorder destruction as safe no-ops, and
  `RequestScope` gives embedders a correct request boundary.
- HTTP requests must carry the correct loopback Host header; concurrent sockets
  are capped at 64; CSP, same-origin policies and permissions policy are sent
  on every response.
- SSE streams send idle heartbeats instead of unchanged analysis documents.
- Added `/api/explain?seq=` and a pinned Event explanation in the UI.
- Added relocatable CMake package config/export, static or shared library
  builds and consumer verification.
- Added a concurrent soak executable, CTest soak smoke and a manual headless
  Chrome UI smoke script.

### Security

- The UI binds only to `127.0.0.1`, validates loopback Host headers, uses a
  128-bit random session token and never exposes arbitrary file paths.
  Mutation endpoints require the token header.
- JSON output validates UTF-8 and rejects non-finite numbers.
- Dump and query readers are bounded before allocation and fuzzed.

### Known limitations

See [LIMITATIONS.md](LIMITATIONS.md). There is no TLS, no remote multi-user
mode, no compression, no Windows backend and no CI configuration.

### Verification

Release, ASan/UBSan and TSan builds pass all five CTest targets, including the
soak smoke. The deterministic fuzz harness completed 50,000 iterations with no
invariant violations, a 30-second soak produced 20,830 records across 1,666
scenarios with 27 flushes and no failures, and a separate CMake consumer
compiled and ran against the installed package. The benchmark suite was run on
an Apple M1 Max. Exact commands, results, LOC and known gaps are in
[FINAL_REPORT.md](FINAL_REPORT.md) and [docs/verification.md](docs/verification.md).
