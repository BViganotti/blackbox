#include "harness.hpp"
#include "support.hpp"

#include <blackbox/crash.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/recorder.hpp>
#include <blackbox/ring.hpp>

#include <csignal>
#include <filesystem>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool count_visitor(void* context, const bb::Record& record) {
  if (!bb::validate_record(record)) {
    return false;
  }
  auto* count = static_cast<std::size_t*>(context);
  ++*count;
  return true;
}

}  // namespace

BB_TEST(ring_signal_visit_reads_published_records) {
  bb::Ring ring(16);
  for (std::uint64_t i = 1; i <= 5; ++i) {
    CHECK(ring.push(bbtest::sample_record(i)));
  }
  std::size_t count = 0;
  const std::size_t visited = ring.for_each_published_signal(&count_visitor, &count);
  CHECK_EQ(visited, 5u);
  CHECK_EQ(count, 5u);
}

BB_TEST(crash_handler_install_and_uninstall) {
  bbtest::TempDir temp;
  bb::Ring first(16);
  std::string error;
  CHECK(!bb::install_crash_handler(first, "", &error));
  CHECK(!error.empty());
  CHECK(!bb::crash_handler_installed());

  error.clear();
  const std::string directory = temp.file("crash");
  CHECK(bb::install_crash_handler(first, directory, &error));
  CHECK(error.empty());
  CHECK(bb::crash_handler_installed());

  bb::Ring second(16);
  std::string second_error;
  CHECK(!bb::install_crash_handler(second, directory, &second_error));
  CHECK(!second_error.empty());

  CHECK(!bb::uninstall_crash_handler(&second));
  CHECK(bb::crash_handler_installed());
  CHECK(bb::uninstall_crash_handler(&first));
  CHECK(!bb::crash_handler_installed());
  CHECK(!bb::uninstall_crash_handler(&first));

  CHECK(bb::install_crash_handler(first, directory, &error));
  CHECK(bb::uninstall_crash_handler(&first));
}

#if !defined(BLACKBOX_ASAN) && !defined(BLACKBOX_TSAN)
BB_TEST(crash_signal_writes_a_readable_dump) {
  bbtest::TempDir temp;
  const std::string directory = temp.file("crash-dumps");
  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(pipe_fds[0]);
    bb::RecorderConfig config;
    config.ring_capacity = 64;
    config.dump_directory = directory;
    bb::Recorder recorder(config);
    if (!recorder.log(bb::Level::Info, "before the crash")) {
      ::_exit(70);
    }
    const auto handle = recorder.begin_span("crashing.span");
    if (!handle.valid() ||
        !recorder.error("database disappeared", bb::Status::Timeout) ||
        !recorder.end_span(handle, bb::Status::Error)) {
      ::_exit(71);
    }
    const char marker = 'R';
    (void)::write(pipe_fds[1], &marker, 1);
    ::close(pipe_fds[1]);
    (void)::raise(SIGSEGV);
    ::_exit(72);
  }

  ::close(pipe_fds[1]);
  struct pollfd descriptor {};
  descriptor.fd = pipe_fds[0];
  descriptor.events = POLLIN;
  const int ready = ::poll(&descriptor, 1, 5000);
  char marker = 0;
  const ssize_t got = ::read(pipe_fds[0], &marker, 1);
  ::close(pipe_fds[0]);
  CHECK_EQ(ready, 1);
  CHECK_EQ(got, 1);
  CHECK_EQ(marker, 'R');

  int status = 0;
  CHECK(::waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGSEGV);

  const auto dumps = bb::list_dump_files(directory);
  CHECK(dumps.size() >= 1u);
  const bb::DumpFileInfo* crash = nullptr;
  for (const auto& dump : dumps) {
    if (dump.valid && dump.meta.flags != 0) {
      crash = &dump;
    }
  }
  CHECK(crash != nullptr);
  if (crash == nullptr) {
    return;
  }
  CHECK((crash->meta.flags & bb::kDumpFlagCrash) != 0);
  CHECK((crash->meta.flags & bb::kDumpFlagInlineStrings) != 0);
  const auto read = bb::read_dump_file(crash->path);
  CHECK(read.ok);
  CHECK(read.complete);
  CHECK(read.records.size() >= 4u);
  bool saw_error = false;
  for (const auto& record : read.records) {
    CHECK(bb::validate_record(record));
    if (record.kind == bb::RecordKind::Error) {
      saw_error = true;
      CHECK_EQ(bb::record_message(record), std::string_view("database disappeared"));
    }
  }
  CHECK(saw_error);
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    CHECK(entry.path().extension() != ".tmp");
  }
}
#endif
