# Blackbox

Blackbox is an always-on causal flight recorder for local C++ services. It keeps
a bounded rolling history of events, spans, logs, metrics, state deltas and
correlation IDs, then lets you inspect the causal chain behind a failure in a
local browser workbench.

Status: under construction.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires C++20, CMake 3.20+, Ninja, and a POSIX platform (macOS first). There are
no third-party dependencies.

## License

MIT. See [LICENSE](LICENSE).
