# Final report — Blackbox 1.0.0

Date: 2026-11-23  
Repository: https://github.com/BViganotti/blackbox  
License: MIT

Blackbox is an always-on causal flight recorder for local C++ services: a
bounded ring of fixed records, causal/state analysis, portable atomic dumps, an
async-signal-safe crash path and an embedded browser workbench served only on
loopback.

## Delivered scope

- **P0 scaffold** — CMake/Ninja, warnings-as-errors, in-repo test harness,
  Makefile, MIT license, no CI (by requirement).
- **P1 model** — trace/span/correlation IDs, bounded attributes, one validated
  record type, string table and record codec with golden/truncation tests.
- **P2 ring** — multi-producer bounded ring, atomic-word seqlock readers, drop
  accounting, per-thread context, concurrent tests.
- **P3 dumps/crash** — atomic `.bbx` writer/reader, strict and recovery modes,
  checksummed frames/footer, signal-safe crash writer and fork test.
- **P4 causal** — acyclic graph, deterministic layout, waterfall, state timeline,
  explain-failure, selected-node explanation, first-divergence comparison and a
  bounded query language.
- **P5 UI** — compiled-in HTML/CSS/JS, token-gated loopback HTTP/SSE, failure-
  focused causal canvas, timeline, waterfall, failure/event/state/dump/compare/
  query inspectors, offline dump opening and pinned explanations.
- **P6 hardening** — deterministic demo, soak harness, fuzzing,
  ASan/UBSan/TSan, benchmarks, relocatable package install, docs and this report.

## LOC

Counted with `wc -l` at the final tree. UI assets are excluded from the C++
budget per requirements.

| Module | Lines |
|---|---:|
| `include/blackbox` public headers | 1,614 |
| `src/core` (trace, ring, recorder, dump, crash, causal, query, JSON) | 5,758 |
| `src/ui` (HTTP, SSE, presentation) | 1,902 |
| `src/demo` (demo service, launcher) | 405 |
| `src/app` (launcher shim, bench, fuzz, soak) | 902 |
| `examples` | 92 |
| **Product/tooling C++ total** | **10,673** |
| Tests (`tests/`) | 3,977 |
| UI assets (`ui/`) — excluded budget | 2,590 |

The product/tooling C++ total is inside the 9–11k target. Tests are additional
and are not counted toward that target.

## Commits

81 commits on `master`, including this report commit. The initial 1.0.0 build
was 60 commits; the hardening pass added dump-contract, retention, metadata,
Host validation, signal-safe encoding, lifetime guards, explain-on-demand,
package export, soak/UI/package smoke tests and the associated docs.

Push target: `origin` → https://github.com/BViganotti/blackbox (public).

## Gaps addressed in the hardening pass

| Previous gap | Resolution |
|---|---|
| Writer/reader dump size mismatch | Shared 256 MiB contract, per-write override, regression test |
| Listing called header-only files valid | Metadata reports header + footer validity separately; UI shows complete vs header-only |
| Retention only by count, prune race | Count/bytes/age retention, just-written file kept, recursive dump guard around list/read/flush |
| No Host validation | Only `127.0.0.1`/`localhost` with the server port accepted; test coverage |
| Unbounded accepted sockets | Hard 64-connection ceiling in addition to queue/worker/SSE caps |
| Crash path used `memcpy` | Signal-safe encoder mode, byte-wise ring reconstruction, lock-free atomic assertions |
| Span use-after-free if Recorder died first | Shared lifetime guard; later span ends are safe no-ops; tested |
| No request boundary helper | `RequestScope` RAII, documented and tested |
| Only latest failure explainable | `/api/explain?seq=` and pinned Event explanation in the UI |
| No CMake package/export/shared option | Relocatable `find_package(blackbox 1.0)`, `bb::blackbox`, VERSION/SOVERSION, static and shared consumption verified |
| No soak/package/UI smoke | `blackbox_soak` (CTest smoke + 30 s run), `tools/package_smoke.sh`, `tools/ui_smoke.sh` |
| Idle SSE resent full snapshots | Idle heartbeat, full refresh only on change or every 20 s |
| TSan race on listener fd/port | Atomic listener/port; re-tested clean |
| No CSP on responses | CSP, COOP/CORP and permissions-policy headers added |

## Verification commands and results

### Release

```sh
cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-verify -j
ctest --test-dir build-verify --output-on-failure
```

- Configure/build: success with `-Wall -Wextra -Wpedantic -Wshadow -Werror`.
- CTest: **5/5 passed** (`unit`, launcher, embedding, version, soak smoke).
- Unit suite: **153/153 checks**.

### ASan + UBSan

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=address
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
./build-asan/blackbox_fuzz --iterations 5000 --seed 271828
```

- CTest: **5/5 passed**.
- Fuzz: 5,000 iterations, no violations.
- The fork/SIGSEGV test compiles out under ASan; the release build runs it.

### TSan

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
./build-tsan/blackbox_bench --quick
```

- CTest: **5/5 passed**.
- No race reports. TSan initially found a listener-fd/port race; it was fixed
  with atomics and the full suite re-run clean.

### Fuzz

```sh
./build-verify/blackbox_fuzz --iterations 50000 --seed 8080
```

- 50,000 iterations, no invariant violations.
- Targets: varint, record codec, query parser/evaluator, dump reader
  (garbage, mutation, truncation).
- Earlier seeds 12345, 777, 424242, 909090 and 20261123 also passed.

### Soak

```sh
./build-verify/blackbox_soak --seconds 30
```

```text
soak: 30 s, 1666 scenarios, 20830 produced, 541 analyses, 27 flushes,
      273650 record copies
OK: soak invariants held
```

Every snapshot record was validated, every graph edge checked forward/acyclic,
queries evaluated, explanations checked, dumps re-read and retention bounded.

### Benchmarks

Host: Apple M1 Max, macOS 26.7.3, arm64, release, APFS temp directory.
`./build-verify/blackbox_bench`:

| Benchmark | Result |
|---|---:|
| Ring single-producer push | 245.49 ns/event · 4,073,486 events/s |
| Ring four-producer push | 447.12 ns/event · 2,236,556 events/s |
| Ring snapshot read | 146.00 ns/record · 6,849,328 records/s |
| Dump write (4096 records, fsync + rename) | 14.97 MiB/s · 4,580 ns/record |
| Dump strict read + validate | 100.87 MiB/s · 679.90 ns/record |
| Query evaluation | 9.02 ns/record |
| Causal build + layout + explain (5000 records) | 1,538.25 µs/trace |
| JSON serialisation | 174.85 MiB/s |
| Ring reserved memory | 16.00 MiB / 8192 records |

### UI and packaging

- `SECONDS_TO_RUN=10 ./tools/ui_smoke.sh build` — launcher printed a loopback
  token URL; headless Chrome rendered the live workbench screenshot; cleanup
  succeeded.
- Live screenshots during development showed the statistics strip, failure-
  focused graph, timeline, event list, findings, waterfall and state inspector.
- A 150-record dump was opened with `--open-dump … --no-demo` and rendered
  without a live stream.
- `./tools/package_smoke.sh build` — a temporary install was consumed by a
  separate CMake project through `find_package(blackbox 1.0)`, compiled, linked
  and printed `1.0.0 records=1`. A shared-library install was also consumed
  manually.
- `test_crash.cpp` forks a child, raises SIGSEGV, validates the written dump and
  confirms the child died from the signal.

## Definition of Done

1. **Release configure succeeds** — verified in `build-verify`.
2. **Build succeeds with zero warnings** — `-Werror` clean.
3. **`ctest` passes** — 5/5 release, ASan and TSan.
4. **Sanitizers documented** — ASan/UBSan and TSan commands/results above.
5. **Launcher opens UI; demo works** — headless Chrome + deterministic demo
   tests and screenshots.
6. **README, docs, CHANGELOG, LICENSE, LIMITATIONS, FINAL_REPORT** — present.
7. **FINAL_REPORT lists LOC, commits, tests, benchmarks, gaps** — this document.
8. **Repo committed and pushed** — `master` pushed to the public remote.
9. **No external dependency, no CI, no CLI-only product** — standard library,
   POSIX/macOS only; no third-party code; no CI configuration; product is the
   local browser workbench.

## Residual constraints (not hidden)

- **CI/CD** is explicitly forbidden by `REQUIREMENTS.md`.
- **Windows** is not a target; POSIX/macOS APIs are the allowed surface.
- **macOS is the only manually exercised platform.** Linux paths exist but were
  not run end-to-end.
- **TLS, remote access, multi-user identity** would require a crypto dependency
  or substantial platform transport work; the product is loopback-only by
  requirement.
- **Automated Safari/Firefox/Chrome matrix** requires a browser-driver
  dependency; the repo has a manual Chrome smoke and real-socket server tests.
- **Framework-specific auto-instrumentation** is not universal; the integration
  guide shows adapter patterns.
- **Sampling/cardinality policy** remains the embedder's responsibility.
- **Trace comparison is positional**, with state-history comparison; no
  insert/delete-aware alignment.
- **The crash signal test is skipped under ASan/TSan**, whose runtimes own fatal
  signal handling; the release fork test covers it.
- **Soak is bounded** (30 s / configurable), not an hours-long production soak.
- **Dumps are unencrypted** and may contain sensitive text; mode 0600 and
  protected storage are required.
- **Explain-failure is heuristic**, not a proof; the raw chain is always shown.

See [LIMITATIONS.md](LIMITATIONS.md), [docs/verification.md](docs/verification.md)
and [docs/testing.md](docs/testing.md) for reproducible details.
