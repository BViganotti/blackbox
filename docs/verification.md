# Verification record

Host: Apple M1 Max, macOS 26.7.3, arm64. Release builds use Apple Clang with
`-O3` and `-Werror`. Sanitizer builds use Debug.

## Release

```sh
cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-verify -j
ctest --test-dir build-verify --output-on-failure
```

Result: clean build, **5/5 CTest targets passed**:

1. `unit` — 153 checks,
2. `launcher_smoke`,
3. `embedding_smoke`,
4. `launcher_version`,
5. `soak_smoke` (2 seconds).

## ASan + UBSan

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=address
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
./build-asan/blackbox_fuzz --iterations 5000 --seed 271828
```

Result: **5/5 passed**, fuzz clean. The fork/SIGSEGV test is compiled out under
ASan because the runtime owns fatal-signal dispositions.

## TSan

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
./build-tsan/blackbox_bench --quick
```

Result: **5/5 passed**, no race reports. A launcher race on the listener fd and
port was found by TSan during hardening, fixed with atomics, and re-tested.

## Fuzz

```sh
./build-verify/blackbox_fuzz --iterations 50000 --seed 8080
```

Result: 50,000 iterations, no invariant violations. Earlier runs with seeds
12345, 777, 424242 and 909090 also passed.

## Soak

```sh
./build-verify/blackbox_soak --seconds 30
```

Result:

```text
soak: 30 s, 1666 scenarios, 20830 produced, 541 analyses, 27 flushes,
      273650 record copies
OK: soak invariants held
```

The soak validates every snapshot record, every graph edge, query evaluation,
explanation shape, dump read and the retention ceiling while the demo produces
traffic continuously.

## UI smoke

```sh
SECONDS_TO_RUN=10 ./tools/ui_smoke.sh build
```

Result: launcher printed a loopback token URL, headless Chrome rendered the live
workbench screenshot, and cleanup removed the temporary directory. This is a
manual macOS Chrome smoke, not a browser matrix.

## Package consumer

```sh
./tools/package_smoke.sh build
```

Result: installed into a temporary prefix, a separate CMake project found
`blackbox 1.0` via `bb::blackbox`, compiled, linked and printed
`1.0.0 records=1`. A shared-library install was also consumed manually with the
installed dylib on the runtime path.

## Benchmarks

`./build-verify/blackbox_bench` on the host above:

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

## Residual constraints

CI/CD is forbidden by `REQUIREMENTS.md`; Windows, TLS, a browser matrix,
framework-specific adapters, sampling/cardinality policy and an hours-long
production soak remain out of scope. See `LIMITATIONS.md`.
