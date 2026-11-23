#!/bin/sh
# Install the built library and compile/run a separate consumer with
# find_package(blackbox). Static build only; shared installs need a runtime path.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build"}
PREFIX=$(mktemp -d "${TMPDIR:-/tmp}/blackbox-install.XXXXXX")
CONSUMER=$(mktemp -d "${TMPDIR:-/tmp}/blackbox-consumer.XXXXXX")
cleanup() { rm -rf "$PREFIX" "$CONSUMER"; }
trap cleanup EXIT INT TERM

cmake --install "$BUILD" --prefix "$PREFIX" >/dev/null
cat >"$CONSUMER/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(blackbox_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(blackbox 1.0 REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE bb::blackbox)
CMAKE
cat >"$CONSUMER/main.cpp" <<'CPP'
#include <blackbox/recorder.hpp>
#include <blackbox/version.hpp>
#include <cstdio>
int main() {
  bb::Recorder recorder(bb::RecorderConfig{32});
  if (!recorder.log(bb::Level::Info, "package consumer")) {
    return 1;
  }
  std::printf("%.*s records=%zu\n",
              static_cast<int>(bb::kVersion.size()), bb::kVersion.data(),
              recorder.snapshot().records.size());
  return recorder.snapshot().records.size() == 1 ? 0 : 1;
}
CPP
cmake -S "$CONSUMER" -B "$CONSUMER/build" -G Ninja \
  -DCMAKE_PREFIX_PATH="$PREFIX" >/dev/null
cmake --build "$CONSUMER/build" >/dev/null
"$CONSUMER/build/consumer"
