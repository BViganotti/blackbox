#pragma once

#include <blackbox/recorder.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace bb {

struct DemoConfig {
  int interval_ms = 1200;
  std::size_t auto_dump_every = 12;
};

/// Synthetic service that exercises the recorder with a deterministic request
/// lifecycle: request -> database timeout -> retry -> circuit breaker open ->
/// fallback, followed by an open-breaker request and a recovery. It runs on its
/// own thread and uses one correlation id per request.
class DemoService {
 public:
  explicit DemoService(Recorder& recorder, DemoConfig config = {});
  ~DemoService();
  DemoService(const DemoService&) = delete;
  DemoService& operator=(const DemoService&) = delete;

  void start();
  void stop();
  bool running() const noexcept { return running_.load(std::memory_order_acquire); }

  /// Requests an immediate scenario before the next interval elapses.
  void trigger();

  /// Number of completed scenarios.
  std::uint64_t scenarios() const noexcept {
    return scenarios_.load(std::memory_order_acquire);
  }

 private:
  void run();
  void generate();

  Recorder& recorder_;
  DemoConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> triggered_{false};
  std::atomic<std::uint64_t> scenarios_{0};
  std::atomic<std::uint64_t> next_index_{0};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool open_breaker_ = false;
  int open_requests_ = 0;
};

/// Runs the browser workbench with the demo service. Options:
///   --no-browser          do not launch the system browser
///   --run-seconds N       exit after N seconds (0 means until SIGINT/SIGTERM)
///   --dump-dir PATH       override the dump directory
///   --capacity N          ring capacity in records (8..131072)
///   --help                print usage
/// Returns a process exit code.
int run_demo_app(int argc, char** argv);

}  // namespace bb
