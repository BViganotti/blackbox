#include <blackbox/recorder.hpp>

#include <blackbox/crash.hpp>

#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

namespace bb {

struct Recorder::ThreadState {
  const Recorder* owner = nullptr;
  std::uint64_t token = 0;
  TraceId trace;
  CorrelationId correlation = kNoCorrelation;
  SpanId current_span = kNoSpan;
  std::uint64_t last_seq = 0;
  std::array<SpanFrame, kMaxSpanDepth> frames{};
  std::size_t depth = 0;
};

Recorder::Recorder(RecorderConfig config)
    : config_(config), ring_(config_.ring_capacity) {
  token_ = random_u64();
  if (token_ == 0) {
    token_ = 1;
  }
  if (config_.install_crash_handlers && !config_.dump_directory.empty()) {
    std::string error;
    crash_installed_ = install_crash_handler(ring_, config_.dump_directory, &error);
    if (!crash_installed_) {
      crash_error_ = error;
    }
  }
}

Recorder::~Recorder() {
  if (crash_installed_) {
    (void)uninstall_crash_handler(&ring_);
  }
}

Recorder::ThreadState& Recorder::state() const {
  static thread_local ThreadState local;
  if (local.owner != this || local.token != token_) {
    local = ThreadState{};
    local.owner = this;
    local.token = token_;
  }
  return local;
}

void Recorder::reject() noexcept {
  rejected_.fetch_add(1, std::memory_order_relaxed);
}

bool Recorder::fill_attributes(Record& record, const Attribute* attrs,
                               std::size_t count) {
  if (count > kMaxAttributesPerRecord) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (!valid_attribute_key(attrs[i].key)) {
      return false;
    }
    store_key(attrs[i].key, record.attrs[i]);
    if (!store_value(attrs[i].value, record.attrs[i].value)) {
      return false;
    }
  }
  record.attr_count = static_cast<std::uint8_t>(count);
  return true;
}

bool Recorder::finalize(Record& record) {
  ThreadState& ctx = state();
  if (record.wall_ns == 0) {
    record.wall_ns = now_wall_ns();
  }
  if (record.mono_ns == 0) {
    record.mono_ns = now_mono_ns();
  }
  if (record.cause_seq == 0) {
    record.cause_seq = ctx.last_seq;
  }
  if (!validate_record(record)) {
    reject();
    return false;
  }
  std::uint64_t assigned = 0;
  if (!ring_.push(record, &assigned)) {
    reject();
    return false;
  }
  ctx.last_seq = assigned;
  return true;
}

bool Recorder::log(Level level, std::string_view message,
                   std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::Log;
  record.level = level;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  if (!set_record_message(record, message) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::note(std::string_view message,
                    std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::Note;
  record.level = Level::Info;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  if (!set_record_message(record, message) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::error(std::string_view message, Status status,
                     std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::Error;
  record.level = Level::Error;
  record.status = status == Status::Unset ? Status::Error : status;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  if (!set_record_message(record, message) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::mark(std::string_view name,
                    std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::Mark;
  record.level = Level::Info;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  if (!set_record_name(record, name) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::metric(std::string_view name, std::int64_t value,
                      std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::Metric;
  record.level = Level::Info;
  record.status = Status::Ok;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  record.int_value = value;
  if (!set_record_name(record, name) ||
      !store_value(Value::integer(value), record.value) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::metric(std::string_view name, double value,
                      std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::Metric;
  record.level = Level::Info;
  record.status = Status::Ok;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  record.double_value = value;
  if (!set_record_name(record, name) ||
      !store_value(Value::floating(value), record.value) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::state_change(std::string_view key, Value previous, Value next,
                            Level level,
                            std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  Record record{};
  record.kind = RecordKind::StateChange;
  record.level = level;
  record.trace = ctx.trace;
  record.span = ctx.current_span;
  record.correlation = ctx.correlation;
  if (!valid_attribute_key(key) || !set_record_state_key(record, key) ||
      !store_value(previous, record.state_old) ||
      !store_value(next, record.state_new) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  return finalize(record);
}

bool Recorder::record(Record record) {
  ThreadState& ctx = state();
  if (record.trace.is_zero()) {
    record.trace = ctx.trace;
  }
  if (record.span == kNoSpan) {
    record.span = ctx.current_span;
  }
  if (record.correlation == kNoCorrelation) {
    record.correlation = ctx.correlation;
  }
  return finalize(record);
}

SpanHandle Recorder::begin_span(std::string_view name,
                                std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  if (ctx.depth >= kMaxSpanDepth) {
    reject();
    return SpanHandle{};
  }
  Record record{};
  record.kind = RecordKind::SpanBegin;
  record.level = Level::Info;
  if (!set_record_name(record, name) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return SpanHandle{};
  }
  if (ctx.trace.is_zero()) {
    ctx.trace = TraceId::random();
  }
  if (ctx.correlation == kNoCorrelation) {
    ctx.correlation = new_correlation_id();
  }
  record.trace = ctx.trace;
  record.span = next_span_id();
  record.parent = ctx.current_span;
  record.correlation = ctx.correlation;
  record.mono_ns = now_mono_ns();
  record.wall_ns = now_wall_ns();
  if (!finalize(record)) {
    return SpanHandle{};
  }
  SpanFrame& frame = ctx.frames[ctx.depth];
  frame.span = record.span;
  frame.start_mono = record.mono_ns;
  std::memcpy(frame.name, record.name, sizeof(frame.name));
  ++ctx.depth;
  ctx.current_span = record.span;
  spans_started_.fetch_add(1, std::memory_order_relaxed);
  return SpanHandle{record.trace, record.span};
}

bool Recorder::end_span(const SpanHandle& handle, Status status,
                        std::initializer_list<Attribute> attrs) {
  ThreadState& ctx = state();
  if (!handle.valid() || ctx.depth == 0 ||
      ctx.frames[ctx.depth - 1].span != handle.span ||
      !(ctx.trace == handle.trace)) {
    spans_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  SpanFrame& frame = ctx.frames[ctx.depth - 1];
  Record record{};
  record.kind = RecordKind::SpanEnd;
  record.level = status == Status::Error || status == Status::Timeout
                     ? Level::Error
                     : Level::Info;
  record.status = status;
  record.trace = handle.trace;
  record.span = handle.span;
  record.correlation = ctx.correlation;
  std::size_t name_length = 0;
  while (name_length < sizeof(frame.name) && frame.name[name_length] != '\0') {
    ++name_length;
  }
  if (!set_record_name(record, std::string_view(frame.name, name_length)) ||
      !fill_attributes(record, attrs.begin(), attrs.size())) {
    reject();
    return false;
  }
  const std::int64_t now = now_mono_ns();
  record.mono_ns = now;
  record.wall_ns = now_wall_ns();
  record.duration_ns = now > frame.start_mono ? now - frame.start_mono : 0;
  if (!finalize(record)) {
    return false;
  }
  --ctx.depth;
  ctx.current_span =
      ctx.depth > 0 ? ctx.frames[ctx.depth - 1].span : kNoSpan;
  spans_ended_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

Span Recorder::span(std::string_view name,
                    std::initializer_list<Attribute> attrs) {
  const SpanHandle handle = begin_span(name, attrs);
  if (!handle.valid()) {
    return Span();
  }
  return Span(this, handle);
}

TraceId Recorder::current_trace() const { return state().trace; }

SpanId Recorder::current_span() const { return state().current_span; }

CorrelationId Recorder::current_correlation() const {
  return state().correlation;
}

std::size_t Recorder::current_depth() const { return state().depth; }

std::uint64_t Recorder::last_sequence() const { return state().last_seq; }

void Recorder::set_correlation(CorrelationId id) noexcept {
  state().correlation = id;
}

void Recorder::reset_thread_context() noexcept {
  ThreadState& ctx = state();
  ctx.trace = TraceId{};
  ctx.correlation = kNoCorrelation;
  ctx.current_span = kNoSpan;
  ctx.last_seq = 0;
  ctx.depth = 0;
}

RingStats Recorder::ring_stats() const { return ring_.stats(); }

RecorderStats Recorder::stats() const {
  RecorderStats stats;
  stats.ring = ring_.stats();
  stats.rejected = rejected_.load(std::memory_order_relaxed);
  stats.spans_started = spans_started_.load(std::memory_order_relaxed);
  stats.spans_ended = spans_ended_.load(std::memory_order_relaxed);
  stats.spans_dropped = spans_dropped_.load(std::memory_order_relaxed);
  return stats;
}

RecorderSnapshot Recorder::snapshot(std::size_t max_records) const {
  RecorderSnapshot snapshot;
  snapshot.ring = ring_.stats();
  const std::size_t cap = std::min(max_records, ring_.capacity());
  if (cap == 0) {
    return snapshot;
  }
  std::vector<Record> buffer(cap);
  const auto result = ring_.read_latest(buffer.data(), cap);
  buffer.resize(result.count);
  snapshot.records = std::move(buffer);
  snapshot.last_seq = result.next_seq;
  if (!snapshot.records.empty()) {
    snapshot.first_seq = snapshot.records.front().seq;
    if (snapshot.first_seq > 0) {
      snapshot.lost = snapshot.first_seq - 1;
    }
  }
  return snapshot;
}

DumpResult Recorder::flush_dump() {
  DumpResult result;
  if (config_.dump_directory.empty()) {
    result.error = "no dump directory configured";
    return result;
  }
  std::lock_guard<std::mutex> guard(dump_mutex_);
  std::error_code directory_error;
  std::filesystem::create_directories(config_.dump_directory, directory_error);
  if (directory_error) {
    result.error = "cannot create dump directory: " + directory_error.message();
    return result;
  }
  const RecorderSnapshot current = snapshot(ring_.capacity());
  std::string suffix;
  try {
    suffix = random_token(3);
  } catch (const std::exception&) {
    suffix = "000000";
  }
  const std::string name = "bbx-" + std::to_string(now_wall_ns()) + "-" +
                           std::to_string(current.last_seq) + "-" + suffix +
                           ".bbx";
  const std::string path =
      (std::filesystem::path(config_.dump_directory) / name).string();
  DumpWriteOptions options;
  options.created_wall_ns = now_wall_ns();
  std::string error;
  if (!write_dump_file(path, current.records, options, &error)) {
    result.error = error;
    return result;
  }
  result.ok = true;
  result.path = path;
  if (config_.max_dumps > 0) {
    const auto dumps = list_dump_files(config_.dump_directory,
                                        config_.max_dumps + 64);
    if (dumps.size() > config_.max_dumps) {
      for (std::size_t i = config_.max_dumps; i < dumps.size(); ++i) {
        std::error_code remove_error;
        std::filesystem::remove(dumps[i].path, remove_error);
      }
    }
  }
  return result;
}

std::vector<DumpFileInfo> Recorder::list_dumps(std::size_t limit) const {
  return list_dump_files(config_.dump_directory, limit);
}

Span::Span(Recorder* recorder, SpanHandle handle) noexcept
    : recorder_(recorder), handle_(handle), active_(true) {}

Span::Span(Span&& other) noexcept
    : recorder_(other.recorder_),
      handle_(other.handle_),
      status_(other.status_),
      active_(other.active_) {
  other.abandon();
}

Span& Span::operator=(Span&& other) noexcept {
  if (this != &other) {
    end();
    recorder_ = other.recorder_;
    handle_ = other.handle_;
    status_ = other.status_;
    active_ = other.active_;
    other.abandon();
  }
  return *this;
}

Span::~Span() { end(); }

bool Span::end() {
  if (!active_ || recorder_ == nullptr) {
    return false;
  }
  active_ = false;
  return recorder_->end_span(handle_, status_);
}

void Span::abandon() noexcept {
  recorder_ = nullptr;
  handle_ = SpanHandle{};
  active_ = false;
}

}  // namespace bb
