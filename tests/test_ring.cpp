#include "harness.hpp"

#include <blackbox/ring.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <array>

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

BB_TEST(ring_read_from_future_is_empty) {
  bb::Ring ring(16);
  for (std::uint64_t i = 1; i <= 5; ++i) {
    CHECK(ring.push(make_record(i)));
  }
  std::vector<bb::Record> records(4);
  const auto result = ring.read_from(UINT64_MAX, records.data(), records.size());
  CHECK_EQ(result.count, 0u);
  CHECK_EQ(result.lost, 0u);
  CHECK_EQ(result.next_seq, UINT64_MAX);
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

BB_TEST(ring_randomized_reads_match_the_retained_window) {
  bb::Xoshiro128 random(0x51A9E);
  const std::array<std::size_t, 4> capacities = {8, 16, 32, 64};
  for (const std::size_t capacity : capacities) {
    for (int round = 0; round < 12; ++round) {
      bb::Ring ring(capacity);
      std::uint64_t produced = 0;
      const std::uint32_t pushes = random.bounded(400);
      for (std::uint32_t i = 0; i < pushes; ++i) {
        CHECK(ring.push(make_record(produced + 1)));
        ++produced;
      }
      CHECK_EQ(ring.newest_seq(), produced);
      const std::uint64_t oldest = produced + 1 > capacity ? produced + 1 - capacity : 1;
      CHECK_EQ(ring.oldest_available(), oldest);
      std::vector<bb::Record> buffer(80);
      for (int read = 0; read < 20; ++read) {
        const std::uint64_t after = random.bounded(
            static_cast<std::uint32_t>(produced + 8));
        const std::size_t max = 1 + random.bounded(80);
        const auto result = ring.read_from(after, buffer.data(), max);
        const std::uint64_t expected_lost =
            after + 1 < oldest ? oldest - 1 - after : 0;
        CHECK_EQ(result.lost, expected_lost);
        const std::uint64_t start = after < oldest - 1 ? oldest - 1 : after;
        const std::uint64_t available = produced > start ? produced - start : 0;
        const std::uint64_t expected_count = std::min<std::uint64_t>(available, max);
        CHECK_EQ(result.count, static_cast<std::size_t>(expected_count));
        for (std::size_t i = 0; i < result.count; ++i) {
          CHECK_EQ(buffer[i].seq, start + i + 1);
          CHECK_EQ(buffer[i].int_value, static_cast<std::int64_t>(start + i + 1));
          CHECK(bb::validate_record(buffer[i]));
        }
        if (result.count > 0) {
          CHECK_EQ(result.next_seq, start + result.count);
        } else {
          CHECK_EQ(result.next_seq, after);
        }
      }
    }
  }
}

BB_TEST(ring_randomized_rejections_do_not_move_sequences) {
  bb::Xoshiro128 random(0xBADF00D);
  bb::Ring ring(32);
  const std::size_t memory = ring.memory_bytes();
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  for (int i = 0; i < 500; ++i) {
    if (random.next_u32() % 5 == 0) {
      bb::Record invalid{};
      invalid.kind = static_cast<bb::RecordKind>(0);
      CHECK(!ring.push(invalid));
      ++rejected;
    } else {
      CHECK(ring.push(make_record(accepted + 1)));
      ++accepted;
    }
    CHECK_EQ(ring.newest_seq(), accepted);
  }
  const auto stats = ring.stats();
  CHECK_EQ(stats.rejected, rejected);
  CHECK_EQ(stats.produced, accepted);
  CHECK_EQ(ring.memory_bytes(), memory);
  CHECK(ring.memory_bytes() <= 32u * (bb::kRecordStorageBytes + 64u));
}
