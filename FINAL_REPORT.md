# Final report — Blackbox 1.0.0

Date: 2026-11-23  
Repository: https://github.com/BViganotti/blackbox  
License: MIT

Blackbox is a local causal flight recorder for C++ services: a bounded ring of
fixed records, causal/state analysis, portable dumps, an async-signal-safe crash
path, and an embedded browser workbench served only on loopback.

## Delivered scope

- **P0 scaffold** — CMake/Ninja, warnings-as-errors, self-contained test harness,
  Makefile, MIT license, no CI.
- **P1 model** — trace/span/correlation IDs, bounded attributes, one validated
  record type, string table and record codec with golden/truncation tests.
- **P2 ring** — multi-producer bounded ring, seqlock readers over atomic words,
  drop accounting, per-thread recording context, concurrent tests.
- **P3 dumps/crash** — atomic `.bbx` writer, strict/recovery reader, checksummed
  frames/footer, async-signal-safe crash writer with alternate stack, fork test.
- **P4 causal** — acyclic graph, deterministic layout, span waterfall, state
  timeline, explain-failure, first-divergence comparison, bounded query language.
- **P5 UI** — compiled-in HTML/CSS/JS workbench, token-gated loopback HTTP/SSE,
  causal canvas with failure focus, timeline, waterfall, inspectors, dumps and
  query bar.
- **P6 demo/hardening** — deterministic failure/recovery demo, offline dump
  opening, compare tab, fuzz harness, ASan/UBSan/TSan runs, benchmarks, docs.

## LOC

Counted with `wc -l` at the final tree. Generated assets are not in the source
tree; the `ui/` assets are excluded from the C++ budget per requirements.

| Module | Lines |
|---|---:|
| `include/blackbox` public headers | 1,561 |
| `src/core` (trace, ring, recorder, dump, crash, causal, query, JSON) | 5,609 |
| `src/ui` (HTTP, SSE, presentation) | 1,766 |
| `src/demo` (demo service, launcher) | 405 |
| `src/app` (launcher shim, bench, fuzz) | 731 |
| `examples` | 92 |
| **Product/tooling C++ total** | **10,164** |
| Tests (`tests/`) | 3,805 |
| UI assets (`ui/`) — excluded from the LOC budget | 2,520 |

The product C++ total is inside the 9–11k target; tests are additional, not
counted toward that target.

## Commits

60 commits on `master`, from scaffold through docs and this report. Every commit
was built and tested before the next one, except two locally amended commits
whose failing test was fixed in the same commit before continuing.

Push target: `origin` → https://github.com/BViganotti/blackbox (public).

## Verification commands and results

### Release

```sh
cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-verify -j
ctest --test-dir build-verify --output-on-failure
```

- Configure: success.
- Build: success with `-Wall -Wextra -Wpedantic -Wshadow -Werror`; no warnings.
- CTest: 4/4 passed (unit suite, launcher smoke, embedding smoke, version).
- Unit suite: 146/146 checks in the last run.

### ASan + UBSan

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=address
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
./build-asan/blackbox_fuzz --iterations 8000 --seed 20261123
```

- CTest: 4/4 passed.
- Fuzz: 8,000 iterations, no invariant violations.
- The fork/crash signal test compiles out under ASan because the runtime owns
  fatal-signal handling; it runs in the release build.

### TSan

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
./build-tsan/blackbox_bench --quick
```

- CTest: 4/4 passed.
- Quick benchmark: completed with no TSan reports.
- The fork/crash signal test compiles out under TSan for the same reason.

### Fuzz

```sh
./build-verify/blackbox_fuzz --iterations 50000 --seed 424242
```

- 50,000 iterations, no invariant violations.
- Targets: varint codec, record codec, query parser/evaluator and dump reader
  (random garbage, mutated valid dumps, truncation).
- Earlier runs also passed with seeds 12345 (30,000), 777 (5,000) and others.

### Benchmarks

Host: Apple M1 Max, macOS 26.7.3, arm64, release, APFS temp directory. Full run
of `./build-verify/blackbox_bench`:

| Benchmark | Result |
|---|---:|
| Ring single-producer push | 236.12 ns/event · 4,235,126 events/s |
| Ring four-producer push | 480.33 ns/event · 2,081,914 events/s |
| Ring snapshot read | 149.01 ns/record · 6,711,160 records/s |
| Dump write (4096 records, fsync + rename) | 15.82 MiB/s · 4,336 ns/record |
| Dump strict read + validate | 102.10 MiB/s · 671.69 ns/record |
| Query evaluation | 9.08 ns/record (71,400 matches in the run) |
| Causal build + layout + explain (5000 records) | 1,478.15 µs/trace |
| Record JSON serialisation | 181.86 MiB/s |
| Ring reserved memory | 16.00 MiB (8192 records) |

Numbers include validation and checksum work; dump write includes durability.

## Demo and UI verification

- The launcher was started with `--no-browser` and a temporary dump directory.
  It printed `http://127.0.0.1:<port>/?token=<32 hex>` and served the UI.
- Headless Chrome loaded the live workbench. The screenshots used during
  verification showed: connected/live SSE, the statistics strip, the focused
  failure chain (10 nodes), the timeline, event list, failure findings and the
  request waterfall with the demo's DB timeout/fallback spans.
- A saved 150-record crash-safe dump was opened with
  `--open-dump … --no-demo`; the banner, recovered records, graph and state
  inspectors rendered without a live server.
- Automated evidence:
  - `test_demo.cpp` asserts the request → two timeouts → breaker open →
    fallback → recovery chain, multiple correlations, automatic dumps and the
    trigger path.
  - `test_server.cpp` uses real sockets for token auth, assets, snapshot,
    query errors, SSE, dump flush/download/open and compare.
  - `test_crash.cpp` forks a child, raises SIGSEGV, validates the written dump and
    confirms the child died from the signal.
- Safari and Firefox were not manually exercised; the UI uses standard APIs
  only.

## Definition of Done

1. **Release configure succeeds** — verified with `build-verify`.
2. **Build succeeds with zero warnings** — `-Werror` clean.
3. **`ctest` passes** — 4/4 in release; 4/4 under ASan and TSan.
4. **Sanitizers pass and are documented** — ASan/UBSan and TSan, above.
5. **Launcher opens the UI; demo works** — launcher tested with headless Chrome;
   demo chain tested and visually rendered.
6. **README, docs, CHANGELOG, LICENSE, LIMITATIONS, FINAL_REPORT exist** — all
   present.
7. **FINAL_REPORT lists LOC, commits, tests, benchmarks, gaps** — this document.
8. **Repo committed and pushed** — `master` pushed to the public GitHub remote.
9. **No external dependency, no CI, no CLI-only product** — standard library,
   POSIX/macOS only; no third-party code; no CI files; the product is the local
   browser workbench.

## Known gaps

- macOS is the only manually exercised platform. POSIX paths exist but Linux was
  not run end-to-end.
- The crash signal test is skipped under ASan/TSan by design; the normal-build
  fork test covers it.
- The browser UI was verified with headless Chrome and a screenshot flow, not a
  full cross-browser automation suite. The Compare tab is covered by server
  tests but was not screenshot-verified.
- SSE sends full snapshots every two seconds rather than deltas; correctness over
  bandwidth for a local tool.
- No TLS, compression, remote access, Windows backend, install packaging beyond
  CMake install rules, or CI.
- Explain-failure is a heuristic; the raw chain is always shown.
- Query language has no regex/aggregation/arithmetic, and trace comparison uses
  positional alignment rather than full sequence alignment.
- Dump retention is count-based (64 by default), not time/size based; crash dumps
  in the same directory share the policy on normal flushes.
- Dumps are unencrypted and may contain sensitive attribute/message text.

See [LIMITATIONS.md](LIMITATIONS.md) for the exhaustive list and
[docs/testing.md](docs/testing.md) for how to reproduce verification.
