#include "harness.hpp"

#include <blackbox/ring.hpp>
#include <blackbox/util/time.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

bb::Record make_record(std::uint64_t value) {
  bb::Record record{};
  record.kind = bb::RecordKind::Log;
  record.wall_ns = static_cast<std::int64_t>(value);
  record.int_value = static_cast<std::int64_t>(value);
  CHECK(bb::set_record_name(record, "event." + std::to_string(value)));
  CHECK(bb::set_record_message(record, "payload " + std::to_string(value)));
  return record;
}

}  // namespace

BB_TEST(ring_capacity_rounding_and_bounds) {
  bb::Ring small(9);
  CHECK_EQ(small.capacity(), 16u);
  bb::Ring minimum(bb::Ring::kMinCapacity);
  CHECK_EQ(minimum.capacity(), bb::Ring::kMinCapacity);
  bool threw = false;
  try {
    bb::Ring bad(1);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    bb::Ring bad(bb::Ring::kMaxCapacity * 2);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

BB_TEST(ring_push_read_roundtrip) {
  bb::Ring ring(64);
  for (std::uint64_t i = 1; i <= 20; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  CHECK_EQ(ring.newest_seq(), 20u);
  CHECK_EQ(ring.oldest_available(), 1u);
  std::vector<bb::Record> records(20);
  const auto result = ring.read_from(0, records.data(), records.size());
  CHECK_EQ(result.count, 20u);
  CHECK_EQ(result.lost, 0u);
  CHECK_EQ(result.next_seq, 20u);
  for (std::size_t i = 0; i < result.count; ++i) {
    CHECK(bb::validate_record(records[i]));
    CHECK_EQ(records[i].seq, i + 1);
    CHECK_EQ(records[i].int_value, static_cast<std::int64_t>(i + 1));
  }
}

BB_TEST(ring_incremental_reads) {
  bb::Ring ring(64);
  for (std::uint64_t i = 1; i <= 10; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  std::vector<bb::Record> records(4);
  auto result = ring.read_from(0, records.data(), records.size());
  CHECK_EQ(result.count, 4u);
  CHECK_EQ(result.next_seq, 4u);
  result = ring.read_from(result.next_seq, records.data(), records.size());
  CHECK_EQ(result.count, 4u);
  CHECK_EQ(result.next_seq, 8u);
  result = ring.read_from(result.next_seq, records.data(), records.size());
  CHECK_EQ(result.count, 2u);
  CHECK_EQ(result.next_seq, 10u);
  result = ring.read_from(result.next_seq, records.data(), records.size());
  CHECK_EQ(result.count, 0u);
  CHECK_EQ(result.lost, 0u);
}

BB_TEST(ring_reports_overwritten_records) {
  bb::Ring ring(8);
  for (std::uint64_t i = 1; i <= 20; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  CHECK_EQ(ring.oldest_available(), 13u);
  std::vector<bb::Record> records(8);
  const auto result = ring.read_from(0, records.data(), records.size());
  CHECK_EQ(result.count, 8u);
  CHECK_EQ(result.lost, 12u);
  CHECK_EQ(result.next_seq, 20u);
  CHECK_EQ(records[0].seq, 13u);
  CHECK_EQ(records[7].seq, 20u);
  CHECK_EQ(ring.stats().overwritten, 12u);
  CHECK_EQ(ring.dropped_before(0), 12u);
  CHECK_EQ(ring.dropped_before(5), 7u);
  CHECK_EQ(ring.dropped_before(12), 0u);
  CHECK_EQ(ring.stats().lost, 12u);
}

BB_TEST(ring_read_latest_returns_newest_window) {
  bb::Ring ring(16);
  for (std::uint64_t i = 1; i <= 40; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  std::vector<bb::Record> records(5);
  const auto result = ring.read_latest(records.data(), records.size());
  CHECK_EQ(result.count, 5u);
  CHECK_EQ(records[0].seq, 36u);
  CHECK_EQ(records[4].seq, 40u);
  CHECK_EQ(ring.dropped_before(0), 24u);
}

BB_TEST(ring_rejects_invalid_records_and_counts_them) {
  bb::Ring ring(16);
  bb::Record invalid{};
  invalid.kind = static_cast<bb::RecordKind>(0);
  CHECK(!ring.push(invalid));
  CHECK_EQ(ring.newest_seq(), 0u);
  const auto stats = ring.stats();
  CHECK_EQ(stats.rejected, 1u);
  CHECK_EQ(stats.produced, 0u);
}

BB_TEST(ring_memory_is_bounded) {
  bb::Ring ring(256);
  const std::size_t bytes = ring.memory_bytes();
  CHECK(bytes > 0);
  for (std::uint64_t i = 0; i < 10'000; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  CHECK_EQ(ring.memory_bytes(), bytes);
  CHECK_EQ(ring.stats().capacity, 256u);
  CHECK_EQ(ring.stats().memory_bytes, bytes);
  CHECK_EQ(ring.newest_seq(), 10'000u);
}

BB_TEST(ring_clear_resets_sequence_and_counters) {
  bb::Ring ring(16);
  for (std::uint64_t i = 1; i <= 5; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  ring.clear();
  CHECK_EQ(ring.newest_seq(), 0u);
  CHECK_EQ(ring.stats().produced, 0u);
  CHECK(ring.push(make_record(1)));
  CHECK_EQ(ring.newest_seq(), 1u);
}

BB_TEST(ring_handles_many_producers) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;
  bb::Ring ring(4096);
  std::atomic<int> ready{0};
  std::atomic<int> push_failures{0};
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&ring, t, &ready, &push_failures] {
      ready.fetch_add(1);
      while (ready.load() < kThreads) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kPerThread; ++i) {
        const auto value = static_cast<std::uint64_t>(t * kPerThread + i);
        if (!ring.push(make_record(value))) {
          push_failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : producers) {
    thread.join();
  }
  CHECK_EQ(push_failures.load(), 0);
  CHECK_EQ(ring.newest_seq(), static_cast<std::uint64_t>(kThreads * kPerThread));
  std::vector<bb::Record> records(static_cast<std::size_t>(kThreads * kPerThread));
  const auto result = ring.read_from(0, records.data(), records.size());
  CHECK_EQ(result.count, records.size());
  CHECK_EQ(result.lost, 0u);
  std::vector<bool> seen(static_cast<std::size_t>(kThreads * kPerThread), false);
  std::uint64_t previous = 0;
  for (std::size_t i = 0; i < result.count; ++i) {
    CHECK(bb::validate_record(records[i]));
    CHECK(records[i].seq > previous);
    previous = records[i].seq;
    const auto value = static_cast<std::size_t>(records[i].int_value);
    CHECK(value < seen.size());
    CHECK(!seen[value]);
    seen[value] = true;
  }
  CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
}

BB_TEST(ring_reader_observes_only_complete_records) {
  constexpr std::uint64_t kTotal = 5000;
  bb::Ring ring(512);
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> observed{0};
  std::atomic<int> read_failures{0};
  std::thread reader([&] {
    std::vector<bb::Record> records(32);
    std::uint64_t cursor = 0;
    std::uint64_t previous = 0;
    while (!done.load(std::memory_order_acquire)) {
      const auto result = ring.read_from(cursor, records.data(), records.size());
      for (std::size_t i = 0; i < result.count; ++i) {
        if (!bb::validate_record(records[i]) || records[i].seq <= previous) {
          read_failures.fetch_add(1);
        }
        previous = records[i].seq;
        observed.fetch_add(1);
      }
      cursor = result.next_seq;
    }
    const auto result = ring.read_from(cursor, records.data(), records.size());
    for (std::size_t i = 0; i < result.count; ++i) {
      if (!bb::validate_record(records[i])) {
        read_failures.fetch_add(1);
      }
    }
  });
  for (std::uint64_t i = 1; i <= kTotal; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  done.store(true, std::memory_order_release);
  reader.join();
  CHECK_EQ(read_failures.load(), 0);
  CHECK(observed.load() > 0);
  CHECK(ring.stats().produced == kTotal);
}
