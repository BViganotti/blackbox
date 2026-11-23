# Testing and verification

Every commit is built with warnings-as-errors and its tests run. The repository
has no CI configuration by design; all commands are reproducible locally.

## Test inventory

`blackbox_tests` is a self-contained harness (no third-party test framework).
Tests are grouped by file:

| File | Coverage |
|---|---|
| `test_version.cpp` | version/build metadata |
| `test_util.cpp` | varint limits/overflow, CRC-32C vectors, time parse/format, durations, RNG, hex |
| `test_model.cpp` | trace ids, span ids, attribute bounds and round trips |
| `test_record.cpp` | fixed record bounds, every enum, validation failures, string setters |
| `test_codec.cpp` | inline and string-table round trips, truncation, corruption, golden bytes |
| `test_ring.cpp` | wrap, loss accounting, bounds, memory ceiling, 4 producers, concurrent reader |
| `test_recorder.cpp` | causal chains, spans, RAII, correlation, state, metrics, depth limit, threads, dumps |
| `test_dump.cpp` | strict/recovery, CRC/header/footer corruption, truncation sweep, listing, random bytes |
| `test_crash.cpp` | install/uninstall, fork + SIGSEGV dump, readable records, no temp files |
| `test_causal.cpp` | DAG edges, roots, unresolved parents, layout, waterfall, explanation, state, divergence |
| `test_query.cpp` | fields, operators, precedence, attributes, trace, errors, limits, fuzz loop |
| `test_json.cpp` | exact output, escaping, invalid UTF-8, depth/byte ceilings, record JSON |
| `test_assets.cpp` | embedded asset lookup and content types |
| `test_server.cpp` | real sockets: auth modes, assets, snapshot, query, dumps, download/open, SSE, limits |
| `test_demo.cpp` | demo failure/recovery chain, correlation spread, automatic dump, trigger |

From the build directory:

```sh
ctest --output-on-failure
```

CTest runs five targets:

- `unit` — 153 checks across the files above,
- `launcher_smoke` — one-second live launcher with a temporary dump directory,
- `embedding_smoke` — headless embedder that records, analyses, dumps and reads,
- `launcher_version` — version output,
- `soak_smoke` — two-second concurrent demo/analyse/flush/retention run.

Longer bounded soak:

```sh
make soak SOAK_SECONDS=60
# or
./build/blackbox_soak --seconds 60
```

The soak validates every snapshot, every cause/span/state graph, query
evaluation, flush, dump read and retention ceiling. It is a two-to-sixty second
smoke, not an hours-long production soak.

### Manual UI smoke

```sh
SECONDS_TO_RUN=10 ./tools/ui_smoke.sh build
```

This targets macOS Chrome by default (`CHROME=...` overrides). It starts the
launcher, verifies the loopback token URL, renders a headless screenshot and
cleans up. It is a developer tool, not a build/runtime dependency.

### Package consumption

```sh
cmake --install build --prefix /tmp/bb-install
# separate project:
find_package(blackbox 1.0 REQUIRED)
target_link_libraries(app PRIVATE bb::blackbox)
```

Static and shared (`-DBLACKBOX_BUILD_SHARED=ON`) installs were both consumed by a
separate CMake project during verification.

## Sanitizers

ASan + UBSan:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=address
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
./build-asan/blackbox_fuzz --iterations 5000 --seed 777
```

TSan:

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
./build-tsan/blackbox_bench --quick
```

The fork/crash test is compiled out under ASan and TSan because those runtimes
install their own fatal-signal machinery. The crash path itself is tested in the
normal build. Results are recorded in `FINAL_REPORT.md`.

## Fuzzing

```sh
./build/blackbox_fuzz --iterations 30000 --seed 12345
```

The harness is deterministic and bounded:

- **varint** — random 64-bit round trips, hostile byte streams, position bounds.
- **codec** — mutation and truncation of a valid encoded record; a successful
  decode must validate.
- **query** — structured and random predicate text; successful parses are
  evaluated against real records; failures must have messages.
- **dump** — random garbage, mutated valid dumps, truncation; checks consumed
  bytes, ordering, per-record validity, failure messages and strict/recovery
  invariants.

A violation prints `FUZZ FAILURE` and exits non-zero.

## Benchmarks

```sh
./build/blackbox_bench
./build/blackbox_bench --quick
```

The benchmark binary measures ring push/read, four-producer contention, dump
write and strict read, query evaluation, causal analysis and JSON serialisation.
It prints measured numbers only; there are no hard-coded results. The README
table records one host run for reference.

## Adding tests

Add `tests/test_*.cpp`; CMake globs the directory. Use `BB_TEST(name)` and the
`CHECK`, `CHECK_EQ`, `CHECK_NE`, `CHECK_NEAR` macros from `tests/harness.hpp`.
`CHECK_EQ` copies its operands before describing them, so temporaries are safe.
Prefer property-style loops over single examples for the ring, dump reader and
query parser.
