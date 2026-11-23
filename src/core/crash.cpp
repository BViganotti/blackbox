#include <blackbox/crash.hpp>

#include <blackbox/codec.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/util/crc32c.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>
#include <blackbox/util/varint.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bb {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "crash handling requires lock-free 64-bit atomics");

constexpr char kHeaderMagic[8] = {'B', 'B', 'X', 'D', 'M', 'P', '0', '1'};
constexpr char kFooterMagic[8] = {'B', 'B', 'X', 'E', 'N', 'D', '0', '1'};
constexpr std::uint32_t kDumpVersion = 1;
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kFooterSize = 32;
constexpr std::uint32_t kFooterEndMarker = 0x42425845u;
constexpr std::uint16_t kCrashFlags =
    kDumpFlagInlineStrings | kDumpFlagCrash;
constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
constexpr std::size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
constexpr std::size_t kPathCapacity = 2048;
constexpr std::size_t kAltStackBytes = 256u * 1024u;

struct HandlerState {
  std::mutex guard;
  std::atomic<Ring*> ring{nullptr};
  std::atomic<bool> active{false};
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  struct sigaction old_actions[kSignalCount] {};
  std::size_t installed_actions = 0;
  stack_t old_stack{};
  bool stack_installed = false;
  int64_t created_wall_ns = 0;
  char final_path[kPathCapacity] {};
  char temp_path[kPathCapacity] {};
  alignas(64) char alt_stack[kAltStackBytes] {};
  alignas(64) std::byte encode_buffer[kMaxEncodedRecordBytes] {};
};

HandlerState& handler_state() {
  static HandlerState state;
  return state;
}

bool write_all(int fd, const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::write(fd, bytes + offset, size - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

void encode_header(std::span<std::byte> out, std::int64_t created_wall_ns,
                   std::uint64_t record_count, std::uint64_t first_seq,
                   std::uint64_t last_seq) noexcept {
  Encoder encoder(out);
  encoder.bytes(kHeaderMagic, sizeof(kHeaderMagic));
  encoder.u32(kDumpVersion);
  encoder.u32(static_cast<std::uint32_t>(kCrashFlags));
  encoder.u32(static_cast<std::uint32_t>(kHeaderSize));
  encoder.u64(static_cast<std::uint64_t>(created_wall_ns));
  encoder.u64(record_count);
  encoder.u64(first_seq);
  encoder.u64(last_seq);
  encoder.u32(0);  // no string table
  encoder.u32(0);
  encoder.u32(crc32c(out.data(), out.size() - 4));
}

struct CrashWriter {
  int fd = -1;
  Crc32c crc;
  std::uint64_t count = 0;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  bool ok = true;
};

bool visit_record(void* raw, const Record& record) noexcept {
  auto& writer = *static_cast<CrashWriter*>(raw);
  HandlerState& state = handler_state();
  Encoder encoder(std::span<std::byte>(state.encode_buffer,
                                      sizeof(state.encode_buffer)),
                  true);
  if (!encode_record(record, encoder, nullptr)) {
    writer.ok = false;
    return false;
  }
  const auto payload = encoder.written();
  std::array<std::byte, 9> frame_header{};
  Encoder header_encoder(frame_header);
  header_encoder.u8(2);  // record frame
  header_encoder.u32(static_cast<std::uint32_t>(payload.size()));
  header_encoder.u32(crc32c(payload));
  if (!header_encoder.ok() ||
      !write_all(writer.fd, frame_header.data(), frame_header.size()) ||
      !write_all(writer.fd, payload.data(), payload.size())) {
    writer.ok = false;
    return false;
  }
  writer.crc.update(payload);
  ++writer.count;
  if (writer.first_seq == 0) {
    writer.first_seq = record.seq;
  }
  writer.last_seq = record.seq;
  return true;
}

void write_crash_dump(Ring& ring, const HandlerState& state) noexcept {
  const int fd = ::open(state.temp_path,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return;
  }
  std::array<std::byte, kHeaderSize> header{};
  encode_header(header, state.created_wall_ns, 0, 0, 0);
  if (!write_all(fd, header.data(), header.size())) {
    ::close(fd);
    ::unlink(state.temp_path);
    return;
  }

  CrashWriter writer;
  writer.fd = fd;
  ring.for_each_published_signal(&visit_record, &writer);
  if (!writer.ok) {
    ::close(fd);
    ::unlink(state.temp_path);
    return;
  }

  encode_header(header, state.created_wall_ns, writer.count, writer.first_seq,
                writer.last_seq);
  if (::lseek(fd, 0, SEEK_SET) != 0 ||
      !write_all(fd, header.data(), header.size()) ||
      ::lseek(fd, 0, SEEK_END) < 0) {
    ::close(fd);
    ::unlink(state.temp_path);
    return;
  }

  std::array<std::byte, kFooterSize> footer{};
  Encoder footer_encoder(footer);
  footer_encoder.bytes(kFooterMagic, sizeof(kFooterMagic));
  footer_encoder.u64(writer.count);
  footer_encoder.u32(writer.crc.value());
  footer_encoder.u32(0);
  footer_encoder.u32(crc32c(footer.data(), 24));
  footer_encoder.u32(kFooterEndMarker);
  if (!footer_encoder.ok() ||
      !write_all(fd, footer.data(), footer.size())) {
    ::close(fd);
    ::unlink(state.temp_path);
    return;
  }
  (void)::fsync(fd);
  (void)::close(fd);
  if (::rename(state.temp_path, state.final_path) != 0) {
    ::unlink(state.temp_path);
  }
}

extern "C" void crash_signal_handler(int signal_number, siginfo_t*,
                                     void*) {
  HandlerState& state = handler_state();
  if (state.active.load(std::memory_order_acquire) &&
      !state.busy.test_and_set(std::memory_order_acquire)) {
    Ring* ring = state.ring.load(std::memory_order_acquire);
    if (ring != nullptr) {
      write_crash_dump(*ring, state);
    }
  }
  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  sigemptyset(&default_action.sa_mask);
  default_action.sa_flags = 0;
  ::sigaction(signal_number, &default_action, nullptr);
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, signal_number);
  (void)::sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
  ::raise(signal_number);
  ::_exit(128 + signal_number);
}

bool copy_path(char (&destination)[kPathCapacity], const std::string& value,
               std::string& error) {
  if (value.empty() || value.size() >= kPathCapacity) {
    error = "crash dump path is empty or too long";
    return false;
  }
  if (value.find('\0') != std::string::npos) {
    error = "crash dump path contains a NUL byte";
    return false;
  }
  std::memcpy(destination, value.data(), value.size());
  destination[value.size()] = '\0';
  return true;
}

std::string sanitize_timestamp(std::string text) {
  for (char& c : text) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9');
    if (!alnum) {
      c = '-';
    }
  }
  return text;
}

}  // namespace

bool install_crash_handler(Ring& ring, std::string_view directory,
                           std::string* error) {
  std::string local_error;
  std::string& err = error != nullptr ? *error : local_error;
  err.clear();
  if (directory.empty()) {
    err = "crash dump directory is empty";
    return false;
  }
  const std::string dir(directory);
  if (dir.find('\0') != std::string::npos) {
    err = "crash dump directory contains a NUL byte";
    return false;
  }
  HandlerState& state = handler_state();
  std::lock_guard<std::mutex> guard(state.guard);
  if (state.active.load(std::memory_order_acquire)) {
    err = "a crash handler is already installed";
    return false;
  }
  std::error_code directory_error;
  std::filesystem::create_directories(dir, directory_error);
  if (directory_error) {
    err = "cannot create crash dump directory: " + directory_error.message();
    return false;
  }
  std::string suffix;
  try {
    suffix = random_token(4);
  } catch (const std::exception&) {
    suffix = "00000000";
  }
  const std::string timestamp = sanitize_timestamp(format_wall_ns(now_wall_ns()));
  const std::string name = "crash-" + timestamp + "-" +
                           std::to_string(static_cast<long long>(::getpid())) +
                           "-" + suffix + ".bbx";
  const std::string final_path =
      (std::filesystem::path(dir) / name).string();
  const std::string temp_path = final_path + ".tmp";
  if (!copy_path(state.final_path, final_path, err) ||
      !copy_path(state.temp_path, temp_path, err)) {
    return false;
  }
  state.created_wall_ns = now_wall_ns();
  state.busy.clear(std::memory_order_release);

  stack_t stack {};
  stack.ss_sp = state.alt_stack;
  stack.ss_size = sizeof(state.alt_stack);
  stack.ss_flags = 0;
  if (::sigaltstack(&stack, &state.old_stack) != 0) {
    err = std::string("sigaltstack failed: ") + std::strerror(errno);
    return false;
  }
  state.stack_installed = true;

  std::size_t installed = 0;
  for (; installed < kSignalCount; ++installed) {
    struct sigaction action {};
    action.sa_sigaction = &crash_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    if (::sigaction(kSignals[installed], &action,
                    &state.old_actions[installed]) != 0) {
      err = std::string("sigaction failed: ") + std::strerror(errno);
      break;
    }
  }
  if (installed != kSignalCount) {
    for (std::size_t i = 0; i < installed; ++i) {
      (void)::sigaction(kSignals[i], &state.old_actions[i], nullptr);
    }
    (void)::sigaltstack(&state.old_stack, nullptr);
    state.stack_installed = false;
    return false;
  }
  state.installed_actions = installed;
  state.ring.store(&ring, std::memory_order_release);
  state.active.store(true, std::memory_order_release);
  return true;
}

bool uninstall_crash_handler(Ring* expected_ring) {
  HandlerState& state = handler_state();
  std::lock_guard<std::mutex> guard(state.guard);
  if (!state.active.load(std::memory_order_acquire)) {
    return false;
  }
  if (expected_ring != nullptr &&
      state.ring.load(std::memory_order_acquire) != expected_ring) {
    return false;
  }
  state.active.store(false, std::memory_order_release);
  state.ring.store(nullptr, std::memory_order_release);
  for (std::size_t i = 0; i < state.installed_actions; ++i) {
    (void)::sigaction(kSignals[i], &state.old_actions[i], nullptr);
  }
  state.installed_actions = 0;
  if (state.stack_installed) {
    (void)::sigaltstack(&state.old_stack, nullptr);
    state.stack_installed = false;
  }
  return true;
}

bool crash_handler_installed() noexcept {
  return handler_state().active.load(std::memory_order_acquire);
}

}  // namespace bb
