#pragma once

#include <blackbox/attribute.hpp>
#include <blackbox/ids.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/record.hpp>
#include <blackbox/ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bb {

/// Recorder configuration. Immutable after construction.
struct RecorderConfig {
  /// Bounded ring capacity in records. Memory use is O(capacity).
  std::size_t ring_capacity = Ring::kDefaultCapacity;
  /// Directory for flushed dumps. Empty disables flush_dump().
  std::string dump_directory;
  /// Install the async-signal-safe crash handler when dump_directory is set.
  bool install_crash_handlers = true;
  /// Oldest flushed dumps beyond this count are removed after a successful
  /// flush. Zero disables the count rule.
  std::size_t max_dumps = 64;
  /// Total bytes allowed across retained dumps. The just-written dump is always
  /// kept even when it alone exceeds the budget. Zero disables the byte rule.
  std::uint64_t max_dump_bytes = 512ull * 1024ull * 1024ull;
  /// Remove dumps older than this many seconds. Zero disables the age rule.
  std::uint64_t max_dump_age_seconds = 0;

  RecorderConfig() = default;
  explicit RecorderConfig(std::size_t capacity) : ring_capacity(capacity) {}
};

/// Outcome of writing a dump.
struct DumpResult {
  bool ok = false;
  std::string path;
  std::string error;
};

/// Combined recorder and ring counters.
struct RecorderStats {
  RingStats ring;
  std::uint64_t rejected = 0;       ///< records refused before entering the ring
  std::uint64_t spans_started = 0;  ///< successful begin_span calls
  std::uint64_t spans_ended = 0;    ///< successful end_span calls
  std::uint64_t spans_dropped = 0;  ///< end_span calls that could not be matched
};

/// Identifies one active span. Handles are values and remain valid only while
/// the span is the innermost active span on the same thread.
struct SpanHandle {
  TraceId trace;
  SpanId span = kNoSpan;

  bool valid() const noexcept { return !trace.is_zero() && span != kNoSpan; }
};

class Recorder;

/// Result of reading a window of records from a recorder.
struct RecorderSnapshot {
  std::vector<Record> records;  ///< ascending sequence order
  RingStats ring;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  std::uint64_t lost = 0;  ///< records produced before first_seq (overwritten or outside the window)
};

/// RAII span. The destructor ends the span with the last status set through
/// set_status(). A Span must not outlive its Recorder.
class Span {
 public:
  Span() noexcept = default;
  Span(Span&& other) noexcept;
  Span& operator=(Span&& other) noexcept;
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;
  ~Span();

  explicit operator bool() const noexcept { return active_; }
  bool active() const noexcept { return active_; }
  const SpanHandle& handle() const noexcept { return handle_; }
  void set_status(Status status) noexcept { status_ = status; }

  /// Ends the span now. Returns false if it was already ended or could not be
  /// matched with the thread context.
  bool end();

 private:
  friend class Recorder;
  Span(Recorder* recorder, SpanHandle handle) noexcept;
  void abandon() noexcept;

  Recorder* recorder_ = nullptr;
  SpanHandle handle_;
  Status status_ = Status::Ok;
  bool active_ = false;
};

/// Thread-safe, allocation-free (after construction) causal recorder.
///
/// Trace context is thread-local: logs, metrics and spans emitted by one thread
/// are automatically linked to that thread's current span and correlation id.
/// Every emitted record receives the sequence of the previous record on the
/// same thread as `cause_seq`, producing a causal chain.
///
/// Lifetime: the recorder must outlive all Span objects created from it.
/// Destruction is safe only when no other thread is using the recorder.
class Recorder {
 public:
  /// Creates the ring. Throws std::invalid_argument if the capacity is outside
  /// the ring bounds and std::bad_alloc if memory is unavailable.
  explicit Recorder(RecorderConfig config = {});
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  const RecorderConfig& config() const noexcept { return config_; }
  RingStats ring_stats() const;
  RecorderStats stats() const;

  /// Appends a log line. Returns false if the message is too long, an attribute
  /// is invalid, or the record is rejected.
  bool log(Level level, std::string_view message,
           std::initializer_list<Attribute> attrs = {});

  /// Appends an informational note. Same failure rules as log().
  bool note(std::string_view message,
            std::initializer_list<Attribute> attrs = {});

  /// Appends an error record at Error level. A status of Unset is promoted to
  /// Error.
  bool error(std::string_view message, Status status = Status::Error,
             std::initializer_list<Attribute> attrs = {});

  /// Appends a named timeline marker.
  bool mark(std::string_view name, std::initializer_list<Attribute> attrs = {});

  /// Records an integer metric. The value is available as int_value and as a
  /// stored Int value.
  bool metric(std::string_view name, std::int64_t value,
              std::initializer_list<Attribute> attrs = {});

  /// Records a floating point metric. Non-finite values are rejected.
  bool metric(std::string_view name, double value,
              std::initializer_list<Attribute> attrs = {});

  /// Records a state transition. `key` must be a valid attribute key. Old and
  /// new values are stored verbatim and must be storable.
  bool state_change(std::string_view key, Value previous, Value next,
                    Level level = Level::Info,
                    std::initializer_list<Attribute> attrs = {});

  /// Low-level append. Fills zero trace/span/correlation/cause fields from the
  /// thread context, timestamps zero timestamps, validates and pushes.
  bool record(Record record);

  /// Starts a span on the current thread. Returns an invalid handle if the
  /// name, attributes or nesting depth are invalid. The new span becomes the
  /// current span until end_span() or the returned Span's destructor runs.
  SpanHandle begin_span(std::string_view name,
                        std::initializer_list<Attribute> attrs = {});

  /// Ends the innermost span if it matches `handle`. Restores the parent span.
  /// Returns false for stale or mismatched handles.
  bool end_span(const SpanHandle& handle, Status status = Status::Ok,
                std::initializer_list<Attribute> attrs = {});

  /// Starts a span and returns an RAII guard.
  Span span(std::string_view name,
            std::initializer_list<Attribute> attrs = {});

  // Thread-local context. Each thread has its own values.
  TraceId current_trace() const;
  SpanId current_span() const;
  CorrelationId current_correlation() const;
  std::size_t current_depth() const;
  std::uint64_t last_sequence() const;

  /// Sets the correlation id for this thread. Zero clears it. Does not change
  /// the current trace or causal chain.
  void set_correlation(CorrelationId id) noexcept;

  /// Clears the entire thread context, including any active spans. Only call
  /// when the thread is between requests; active Span guards must not be used
  /// afterwards.
  void reset_thread_context() noexcept;

  /// Copies the newest `max_records` records in ascending order.
  RecorderSnapshot snapshot(std::size_t max_records = 2048) const;

  /// Writes every retained record to an atomically renamed file in the
  /// configured dump directory. Serialised with other flushes. Returns an
  /// error when no directory is configured or the write fails.
  DumpResult flush_dump();

  /// Lists dump files in the configured directory, newest first. Returns an
  /// empty vector when no directory is configured.
  std::vector<DumpFileInfo> list_dumps(std::size_t limit = 64) const;

  /// Serialises dump-directory reads against flushing/pruning. Server handlers
  /// hold this guard while listing and reading a file so retention cannot
  /// unlink it mid-request. The mutex is recursive, so list_dumps() may be
  /// called while the guard is held.
  std::unique_lock<std::recursive_mutex> lock_dumps() const;

  /// True when this recorder installed the process crash handler. A failed
  /// installation is reported through crash_error() but does not fail
  /// construction: recording continues without crash dumps.
  bool crash_handler_active() const noexcept { return crash_installed_; }

  /// Empty when crash handler installation succeeded or was not requested.
  const std::string& crash_error() const noexcept { return crash_error_; }

 private:
  struct SpanFrame {
    SpanId span = kNoSpan;
    std::int64_t start_mono = 0;
    char name[kMaxNameBytes + 1] = {};
  };
  static constexpr std::size_t kMaxSpanDepth = 64;

  struct ThreadState;
  ThreadState& state() const;
  bool fill_attributes(Record& record, const Attribute* attrs,
                       std::size_t count);
  bool finalize(Record& record);
  void reject() noexcept;

  RecorderConfig config_;
  Ring ring_;
  std::uint64_t token_ = 0;
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> spans_started_{0};
  std::atomic<std::uint64_t> spans_ended_{0};
  std::atomic<std::uint64_t> spans_dropped_{0};
  mutable std::recursive_mutex dump_mutex_;
  bool crash_installed_ = false;
  std::string crash_error_;
};

}  // namespace bb
