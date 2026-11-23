BUILD ?= build
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all build test asan tsan release bench fuzz run clean

all: build

build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j $(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure

asan:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=address
	cmake --build build-asan -j $(JOBS)
	ctest --test-dir build-asan --output-on-failure

tsan:
	cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLACKBOX_SANITIZE=thread
	cmake --build build-tsan -j $(JOBS)
	ctest --test-dir build-tsan --output-on-failure

bench: build
	./$(BUILD)/blackbox_bench

fuzz: build
	./$(BUILD)/blackbox_fuzz --iterations 20000

run: build
	./$(BUILD)/blackbox_launcher

clean:
	rm -rf build build-asan build-tsan build-undef
