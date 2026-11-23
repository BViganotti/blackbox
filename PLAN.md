# Blackbox — Implementation Plan

Status: ready for one-shot implementation in this repository.

## Mission
Blackbox is an always-on causal flight recorder for local C++ services. It keeps
a bounded rolling history of structured events, spans, logs, metrics, state deltas
and correlation IDs. When something goes wrong, the developer opens the last N
minutes and sees a causal graph: request → retry → timeout → fallback, with the
state change that triggered each step. It is a local desktop workbench, not a CLI
and not a toy.

## Product shape
- Embeddable `blackbox` C++ library.
- Portable trace bundles that can be opened offline.
- Local UI workbench: causal graph, timeline, request waterfall, state changes,
  query panel and dump browser.
- Demo service: request → DB timeout → retry → circuit breaker → fallback.

## Architecture
```
blackbox/
  include/blackbox/
    recorder.hpp, span.hpp, event.hpp, state.hpp, dump.hpp
  src/
    trace/       model, string table, varint, CRC, writer/reader, indexes
    ring/        bounded MPSC ring, sequence numbers, drop accounting
    crash/       preallocated dump, async-signal-safe writer, recovery
    causal/      parent/child, correlation IDs, graph, explain-failure
    query/       predicate parser and evaluator
    ui/          HTTP/SSE, JSON, embedded assets, launcher
  ui/            original HTML/CSS/JS assets, embedded at build time
  examples/blackbox_demo/
  tests/  fuzz/  bench/
```

## LOC budget (target ~10k)
| Module | LOC |
|---|---:|
| trace format/writer/reader | 1,400 |
| ring buffer + drop accounting | 1,000 |
| public API + causal model | 1,100 |
| crash dump/recovery | 1,000 |
| state deltas + analysis | 1,200 |
| query language | 700 |
| UI server + assets | 1,900 |
| launcher + demo | 600 |
| tests/fuzz/bench | 1,100 |

## Milestones and commit ranges
### P0 Scaffold (commits 1–8)
CMake, test harness, README, LICENSE, Makefile, directory layout, no CI, no dependencies.

### P1 Event/span/state model (commits 9–18)
Attribute variant → correlation IDs → causal parent/child → state deltas → trace model → serialization → golden tests.

### P2 Ring buffer (commits 19–28)
Bounded MPSC ring → sequence numbers → drop policy/accounting → per-thread fast path → contention benchmark → crash-safe snapshot.

### P3 Crash path (commits 29–38)
Preallocated buffers → async-signal-safe writer → atomic rename → checksum → footer/recovery → signal tests → truncation/fuzz.

### P4 Causal analysis (commits 39–48)
DAG construction → deterministic layout → explain-failure walk → state transition timeline → query evaluator → first-divergence helpers.

### P5 UI (commits 49–62)
HTTP/JSON/SSE → embedded assets → causal graph canvas → timeline → waterfall → state inspector → query bar → dump browser.

### P6 Demo and hardening (commits 63–80)
Demo service → recorded failure scenarios → dump/reopen → malformed input fuzz → ASan/UBSan → TSan → benchmarks → docs → FINAL_REPORT.

## Testing
- Golden trace round-trip and causal reconstruction.
- Property tests for ring ordering, drop accounting and state deltas.
- Signal/crash dump tests in a child process.
- Fuzz dump reader and query parser.
- Memory-bound tests for the ring.

## Acceptance criteria
- Signal dump of a running process opens and validates.
- Ring never exceeds configured memory; drop counts are visible.
- Causal graph reconstructs parent chains from the trace alone.
- State view shows old/new values and the causal event.
- Explain-failure walks from error to root cause in the demo.
- Malformed/truncated dumps fail cleanly.

## Risks and fallbacks
- Crash handlers are platform-sensitive: macOS first, preallocated buffer only.
- Causal graph ambiguity: preserve raw parent IDs and show unresolved edges.
- UI scope: local web UI with canvas is the required baseline.
