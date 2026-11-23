#pragma once

#include <blackbox/record.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace bb {

/// Snapshot of ring counters. All values are monotonic except the sequence
/// window, which moves forward.
struct RingStats {
  std::uint64_t capacity = 0;
  std::uint64_t memory_bytes = 0;
  std::uint64_t produced = 0;     ///< records assigned a sequence
  std::uint64_t rejected = 0;     ///< pushes refused because the record was invalid
  std::uint64_t overwritten = 0;  ///< slot overwrites of previously published records
  std::uint64_t lost = 0;         ///< records missed, summed over all read calls
  std::uint64_t oldest_seq = 0;   ///< oldest sequence still readable, 0 if empty
  std::uint64_t newest_seq = 0;   ///< most recently published sequence, 0 if empty
};

/// Bounded multi-producer flight ring. Producers are serialised by a small
/// spin lock and never allocate; readers are lock-free and use a per-slot
/// sequence check so they can never return a torn record. Payload words are
/// atomics, which keeps the structure free of C++ data races and therefore TSan
/// clean.
///
/// Behaviour when full: the oldest slot is overwritten. Producers never block
/// waiting for a reader. Readers detect the overwrite and report `lost` in the
/// read result. Memory use is exactly capacity * sizeof(Slot) and never grows.
///
/// Thread-safety: push() may be called from any number of threads. read_from()/
/// read_latest() may be called concurrently with push() and with each other.
/// clear() requires exclusive access.
class Ring {
 public:
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "the crash path requires lock-free 64-bit atomics");
  static constexpr std::size_t kMinCapacity = 8;
  static constexpr std::size_t kMaxCapacity = 1u << 17;
  static constexpr std::size_t kDefaultCapacity = 4096;

  /// Creates a ring. `capacity` is rounded up to a power of two and clamped to
  /// [kMinCapacity, kMaxCapacity]; out-of-range values throw
  /// std::invalid_argument rather than silently changing behaviour.
  explicit Ring(std::size_t capacity = kDefaultCapacity);
  ~Ring();
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  /// Validates and appends a record. The ring assigns record.seq. Returns false
  /// for an invalid record and increments the rejected counter.
  /// Validates and appends a record. The ring assigns record.seq. Returns false
  /// for an invalid record and increments the rejected counter. When
  /// `assigned_seq` is non-null it receives the assigned sequence.
  bool push(const Record& record, std::uint64_t* assigned_seq = nullptr);

  struct ReadResult {
    std::size_t count = 0;     ///< records written to the output array
    std::uint64_t lost = 0;    ///< records skipped before this read
    std::uint64_t next_seq = 0;///< sequence of the last returned record
  };

  /// Reads up to `max` records with sequence numbers greater than `after_seq`.
  /// Pass 0 to start from the oldest retained record. If older records were
  /// overwritten, `lost` reports how many. Reading stops early if a record is
  /// being written or was overwritten during the read; pass result.next_seq to
  /// continue.
  ReadResult read_from(std::uint64_t after_seq, Record* out,
                       std::size_t max) const;

  /// Reads up to `max` of the newest records in ascending order.
  ReadResult read_latest(Record* out, std::size_t max) const;

  /// Number of records that would be skipped by a read after `after_seq`.
  std::uint64_t dropped_before(std::uint64_t after_seq) const;

  std::uint64_t oldest_available() const noexcept;
  std::uint64_t newest_seq() const noexcept;
  std::uint64_t next_seq() const noexcept;

  RingStats stats() const;
  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t memory_bytes() const noexcept;

  /// Resets counters and the sequence window. Requires exclusive access.
  void clear();

  /// Visits every currently readable record in ascending sequence order without
  /// allocating or taking a lock. Returns the number of visited records. The
  /// visitor returns false to stop early. This method is async-signal-safe when
  /// called from a handler while no other thread destroys the ring; it is used
  /// by the crash dumper. Records overwritten during the walk are skipped.
  using SignalVisitor = bool (*)(void* context, const Record& record);
  std::size_t for_each_published_signal(SignalVisitor visitor,
                                        void* context) const noexcept;

 private:
  static constexpr std::size_t kRecordWords = (sizeof(Record) + 7) / 8;

  struct Slot {
    Slot() noexcept : stamp(0) {
      for (auto& word : words) {
        word.store(0, std::memory_order_relaxed);
      }
    }
    alignas(64) std::atomic<std::uint64_t> stamp;
    std::atomic<std::uint64_t> words[kRecordWords];
  };

  bool read_slot(std::uint64_t seq, Record& out) const noexcept;
  bool read_slot_signal(std::uint64_t seq, Record& out) const noexcept;
  std::uint64_t oldest_for(std::uint64_t next) const noexcept;
  void lock() noexcept;
  void unlock() noexcept;

  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  std::unique_ptr<Slot[]> slots_;
  std::atomic<std::uint64_t> next_seq_{1};
  std::atomic<std::uint64_t> overwritten_{0};
  std::atomic<std::uint64_t> rejected_{0};
  mutable std::atomic<std::uint64_t> lost_total_{0};
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

}  // namespace bb
