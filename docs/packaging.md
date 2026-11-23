# Building, installing and consuming Blackbox

Blackbox is a CMake project with no package manager, vendored code or
third-party library dependency.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The static library is the default. A shared library with `VERSION 1.0.0` and
`SOVERSION 1` is available:

```sh
cmake -S . -B build-shared -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBLACKBOX_BUILD_SHARED=ON
cmake --build build-shared -j
```

## Install

```sh
cmake --install build --prefix /usr/local
```

Installs:

```text
include/blackbox/...                         public headers
lib/libblackbox.a (or libblackbox.1.dylib)   library
lib/cmake/blackbox/blackboxConfig.cmake      package config
lib/cmake/blackbox/blackboxTargets.cmake     imported targets
bin/blackbox_launcher                        workbench launcher
share/doc/blackbox/...                       README, license, changelog, limits
```

## Consume

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_service LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(blackbox 1.0 REQUIRED)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE bb::blackbox)
```

`bb::blackbox` carries the install-interface include directory and the
`Threads::Threads` dependency. `bb::blackbox_options` carries the public version
and platform definitions. Build-only `-Wall/-Wextra/-Werror` and sanitizer flags
are wrapped in `BUILD_INTERFACE` and do **not** leak into consumers.

The `VERSION` header is self-contained:

```cpp
#include <blackbox/version.hpp>
std::printf("%.*s\n", static_cast<int>(bb::kVersion.size()), bb::kVersion.data());
```

Static and shared installs were both consumed by a separate CMake project in
verification; the shared consumer needs the installed library on its runtime
search path (`DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH` on macOS/ELF, or an rpath).

## ABI and versioning

- The library is versioned 1.0.0 with `SameMajorVersion` package compatibility.
- Public structs (`Record`, `Attribute`, `Value`, `DumpMetadata`) are exposed
  directly; their layout is not yet an ABI guarantee across major versions.
- Dump format version 1 is independent of the library SONAME. A future format
  change increments the dump version and must be rejected cleanly by old
  readers.
- There is no package signing, notarization, Homebrew formula or Linux package
  in this repository. Those need external release infrastructure.

## Cross-compilation

The code relies on POSIX sockets, `fork`, signals, `sigaltstack`, `fsync`,
`getentropy` and filesystem APIs. macOS is the verified target. Linux should
build with a POSIX toolchain but was not exercised in this environment. Windows
is not a target under `REQUIREMENTS.md`.
