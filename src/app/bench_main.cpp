#include <blackbox/record.hpp>
#include <blackbox/ring.hpp>
#include <blackbox/util/time.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;

bb::Record make_record(std::uint64_t value) {
  bb::Record record{};
  record.kind = bb::RecordKind::Log;
  record.level = bb::Level::Info;
  record.trace = bb::TraceId{0xB0B0B0B0ull, value + 1};
  record.correlation = value + 1;
  record.cause_seq = value > 1 ? value - 1 : 0;
  record.wall_ns = 1'700'000'000'000'000'000ll + static_cast<std::int64_t>(value);
  record.mono_ns = static_cast<std::int64_t>(value);
  record.attr_count = 2;
  bb::store_key("bench.index", record.attrs[0]);
  (void)bb::store_value(bb::Value::integer(static_cast<std::int64_t>(value)),
                        record.attrs[0].value);
  bb::store_key("bench.kind", record.attrs[1]);
  (void)bb::store_value(bb::Value::string("ring"), record.attrs[1].value);
  (void)bb::set_record_message(record, "benchmark event");
  return record;
}

double ns_per(Clock::duration duration, std::uint64_t operations) {
  if (operations == 0) {
    return 0.0;
  }
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  return static_cast<double>(nanos) / static_cast<double>(operations);
}

void print_row(const char* name, double value, const char* unit) {
  std::printf("%-34s %14.2f %s\n", name, value, unit);
}

void bench_ring_push(bool quick) {
  const std::uint64_t events = quick ? 20'000 : 120'000;
  bb::Ring ring(8192);
  const auto started = Clock::now();
  for (std::uint64_t i = 1; i <= events; ++i) {
    if (!ring.push(make_record(i))) {
      std::printf("ring push failed at %llu\n",
                  static_cast<unsigned long long>(i));
      return;
    }
  }
  const auto elapsed = Clock::now() - started;
  const double ns = ns_per(elapsed, events);
  const double per_second = 1e9 / ns;
  if (ring.newest_seq() != events) {
    std::printf("ring sequence mismatch\n");
    return;
  }
  print_row("ring: single-producer push", ns, "ns/event");
  print_row("ring: single-producer rate", per_second, "events/s");
}

void bench_ring_contention(bool quick) {
  constexpr int kThreads = 4;
  const std::uint64_t per_thread = quick ? 10'000 : 40'000;
  bb::Ring ring(16'384);
  std::atomic<int> ready{0};
  std::atomic<std::uint64_t> checksum{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  const auto started = Clock::now();
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ring, &ready, &checksum, t, per_thread] {
      ready.fetch_add(1);
      while (ready.load() < kThreads) {
        std::this_thread::yield();
      }
      std::uint64_t local = 0;
      for (std::uint64_t i = 0; i < per_thread; ++i) {
        if (ring.push(make_record(static_cast<std::uint64_t>(t) * 1'000'000 + i))) {
          local += i;
        }
      }
      checksum.fetch_add(local, std::memory_order_relaxed);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto elapsed = Clock::now() - started;
  const std::uint64_t total = per_thread * kThreads;
  const double ns = ns_per(elapsed, total);
  g_sink += checksum.load();
  print_row("ring: four-producer push", ns, "ns/event");
  print_row("ring: four-producer rate", 1e9 / ns, "events/s");
}

void bench_ring_read(bool quick) {
  bb::Ring ring(8192);
  for (std::uint64_t i = 1; i <= 8192; ++i) {
    (void)ring.push(make_record(i));
  }
  std::vector<bb::Record> buffer(8192);
  const std::uint64_t passes = quick ? 20 : 100;
  std::uint64_t read = 0;
  const auto started = Clock::now();
  for (std::uint64_t pass = 0; pass < passes; ++pass) {
    const auto result = ring.read_from(0, buffer.data(), buffer.size());
    read += result.count;
    g_sink += buffer[result.count / 2].seq;
  }
  const auto elapsed = Clock::now() - started;
  const double ns = ns_per(elapsed, read);
  print_row("ring: snapshot read", ns, "ns/record");
  print_row("ring: snapshot rate", 1e9 / ns, "records/s");
}

void bench_ring_memory() {
  bb::Ring ring(8192);
  const auto stats = ring.stats();
  print_row("ring: reserved memory", static_cast<double>(stats.memory_bytes) /
                                        (1024.0 * 1024.0),
            "MiB");
  print_row("ring: capacity", static_cast<double>(stats.capacity), "records");
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      quick = true;
    } else {
      std::printf("usage: blackbox_bench [--quick]\n");
      return 2;
    }
  }
  std::printf("blackbox ring benchmark (%s)\n", quick ? "quick" : "full");
  std::printf("%-34s %14s %s\n", "benchmark", "value", "unit");
  bench_ring_memory();
  bench_ring_push(quick);
  bench_ring_contention(quick);
  bench_ring_read(quick);
  std::printf("checksum %llu\n",
              static_cast<unsigned long long>(g_sink));
  return 0;
}
