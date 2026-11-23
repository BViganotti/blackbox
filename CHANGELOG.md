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
  parsing, fixed worker pool, snapshot/query/dump/SSE endpoints.
- Deterministic demo service: request → two DB timeouts → breaker open →
  cached fallback → open-breaker request → recovery, with automatic dumps.
- Launcher that prints a token URL and opens the system browser; `--open-dump`
  for offline bundles and `--no-demo` for inspection.
- Unit, property, malformed-input, fork/crash and real-socket server tests.
- Deterministic fuzz harness for dumps, queries, record codec and varints.
- Benchmarks for ring, dump, query, causal analysis and JSON.
- Documentation: architecture, dump format, query language, embedding, crash
  safety and testing; MIT license.

### Security

- The UI binds only to `127.0.0.1`, uses a 128-bit random session token and
  never exposes arbitrary file paths. Mutation endpoints require the token
  header.
- JSON output validates UTF-8 and rejects non-finite numbers.
- Dump and query readers are bounded before allocation and fuzzed.

### Known limitations

See [LIMITATIONS.md](LIMITATIONS.md). There is no TLS, no remote multi-user
mode, no compression, no Windows backend and no CI configuration.
