#include <blackbox/ring.hpp>

#include <array>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace bb {
namespace {

std::size_t round_up_power_of_two(std::size_t value) {
  std::size_t result = Ring::kMinCapacity;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

}  // namespace

Ring::Ring(std::size_t capacity) {
  if (capacity < kMinCapacity || capacity > kMaxCapacity) {
    throw std::invalid_argument("ring capacity out of range");
  }
  capacity_ = round_up_power_of_two(capacity);
  mask_ = capacity_ - 1;
  slots_ = std::make_unique<Slot[]>(capacity_);
}

Ring::~Ring() = default;

void Ring::lock() noexcept {
  int spins = 0;
  while (lock_.test_and_set(std::memory_order_acquire)) {
    if (++spins > 64) {
      std::this_thread::yield();
      spins = 0;
    }
  }
}

void Ring::unlock() noexcept { lock_.clear(std::memory_order_release); }

bool Ring::push(const Record& record, std::uint64_t* assigned_seq) {
  if (!validate_record(record)) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Record stored = record;
  lock();
  const std::uint64_t seq = next_seq_.fetch_add(1, std::memory_order_release);
  stored.seq = seq;
  Slot& slot = slots_[seq & mask_];
  const std::uint64_t previous = slot.stamp.load(std::memory_order_relaxed);
  if (previous != 0) {
    overwritten_.fetch_add(1, std::memory_order_relaxed);
  }
  // Seqlock: odd stamp means "being written". Payload words are atomics so
  // there is no data race, only a consistency check.
  slot.stamp.store(seq * 2 + 1, std::memory_order_relaxed);
  std::array<std::uint64_t, kRecordWords> words{};
  std::memcpy(words.data(), &stored, sizeof(Record));
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    slot.words[i].store(words[i], std::memory_order_relaxed);
  }
  slot.stamp.store(seq * 2 + 2, std::memory_order_release);
  if (assigned_seq != nullptr) {
    *assigned_seq = seq;
  }
  unlock();
  return true;
}

bool Ring::read_slot(std::uint64_t seq, Record& out) const noexcept {
  const Slot& slot = slots_[seq & mask_];
  const std::uint64_t stamp = slot.stamp.load(std::memory_order_acquire);
  if (stamp != seq * 2 + 2) {
    return false;
  }
  std::array<std::uint64_t, kRecordWords> words{};
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    words[i] = slot.words[i].load(std::memory_order_relaxed);
  }
  if (slot.stamp.load(std::memory_order_acquire) != stamp) {
    return false;
  }
  std::memcpy(&out, words.data(), sizeof(Record));
  return true;
}

std::uint64_t Ring::oldest_for(std::uint64_t next) const noexcept {
  return next > capacity_ ? next - capacity_ : 1;
}

std::uint64_t Ring::oldest_available() const noexcept {
  return oldest_for(next_seq_.load(std::memory_order_acquire));
}

std::uint64_t Ring::newest_seq() const noexcept {
  const std::uint64_t next = next_seq_.load(std::memory_order_acquire);
  return next == 1 ? 0 : next - 1;
}

std::uint64_t Ring::next_seq() const noexcept {
  return next_seq_.load(std::memory_order_acquire);
}

Ring::ReadResult Ring::read_from(std::uint64_t after_seq, Record* out,
                                 std::size_t max) const {
  ReadResult result;
  result.next_seq = after_seq;
  if (out == nullptr || max == 0) {
    return result;
  }
  const std::uint64_t next = next_seq_.load(std::memory_order_acquire);
  const std::uint64_t newest = next - 1;
  if (newest == 0 || after_seq >= newest) {
    return result;
  }
  const std::uint64_t oldest = oldest_for(next);
  std::uint64_t start = after_seq;
  if (start + 1 < oldest) {
    result.lost = oldest - 1 - start;
    start = oldest - 1;
  }
  if (start >= newest) {
    if (result.lost != 0) {
      lost_total_.fetch_add(result.lost, std::memory_order_relaxed);
    }
    return result;
  }
  for (std::uint64_t seq = start + 1; seq <= newest; ++seq) {
    if (result.count >= max) {
      break;
    }
    if (!read_slot(seq, out[result.count])) {
      break;
    }
    ++result.count;
    result.next_seq = seq;
  }
  if (result.lost != 0) {
    lost_total_.fetch_add(result.lost, std::memory_order_relaxed);
  }
  return result;
}

Ring::ReadResult Ring::read_latest(Record* out, std::size_t max) const {
  ReadResult result;
  if (out == nullptr || max == 0) {
    return result;
  }
  const std::uint64_t next = next_seq_.load(std::memory_order_acquire);
  const std::uint64_t newest = next - 1;
  if (newest == 0) {
    return result;
  }
  const std::uint64_t oldest = oldest_for(next);
  const std::uint64_t available = newest - oldest + 1;
  const auto wanted = static_cast<std::uint64_t>(max);
  const std::uint64_t count = available < wanted ? available : wanted;
  const std::uint64_t start = newest - count + 1;
  return read_from(start - 1, out, max);
}

std::uint64_t Ring::dropped_before(std::uint64_t after_seq) const {
  const std::uint64_t oldest = oldest_available();
  if (after_seq + 1 < oldest) {
    return oldest - 1 - after_seq;
  }
  return 0;
}

RingStats Ring::stats() const {
  RingStats stats;
  const std::uint64_t next = next_seq_.load(std::memory_order_acquire);
  stats.capacity = capacity_;
  stats.memory_bytes = memory_bytes();
  stats.produced = next - 1;
  stats.rejected = rejected_.load(std::memory_order_relaxed);
  stats.overwritten = overwritten_.load(std::memory_order_relaxed);
  stats.lost = lost_total_.load(std::memory_order_relaxed);
  stats.oldest_seq = oldest_for(next);
  stats.newest_seq = next - 1;
  return stats;
}

std::size_t Ring::memory_bytes() const noexcept {
  return capacity_ * sizeof(Slot);
}

std::size_t Ring::for_each_published_signal(SignalVisitor visitor,
                                            void* context) const noexcept {
  if (visitor == nullptr) {
    return 0;
  }
  const std::uint64_t next = next_seq_.load(std::memory_order_acquire);
  const std::uint64_t newest = next - 1;
  if (newest == 0) {
    return 0;
  }
  const std::uint64_t oldest = oldest_for(next);
  std::size_t visited = 0;
  for (std::uint64_t seq = oldest;; ++seq) {
    Record record;
    if (read_slot(seq, record)) {
      if (!visitor(context, record)) {
        break;
      }
      ++visited;
    }
    if (seq == newest) {
      break;
    }
  }
  return visited;
}

void Ring::clear() {
  next_seq_.store(1, std::memory_order_release);
  overwritten_.store(0, std::memory_order_relaxed);
  rejected_.store(0, std::memory_order_relaxed);
  lost_total_.store(0, std::memory_order_relaxed);
}

}  // namespace bb
