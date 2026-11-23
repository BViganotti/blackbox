#include <blackbox/demo.hpp>

#include <blackbox/dump.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>
#include <blackbox/ui_server.hpp>
#include <blackbox/version.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace bb {
namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void stop_signal(int) { g_stop = 1; }

std::string default_dump_directory() {
  if (const char* configured = std::getenv("BLACKBOX_DUMP_DIR");
      configured != nullptr && configured[0] != '\0') {
    return configured;
  }
  const char* home = std::getenv("HOME");
  const std::string base =
      home != nullptr && home[0] != '\0' ? std::string(home) : std::string("/tmp");
#if defined(__APPLE__)
  return (std::filesystem::path(base) / "Library" / "Application Support" /
          "Blackbox" / "dumps")
      .string();
#else
  return (std::filesystem::path(base) / ".local" / "share" / "blackbox" /
          "dumps")
      .string();
#endif
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc() && result.ptr == end;
}

struct AppOptions {
  bool no_browser = false;
  bool no_demo = false;
  bool dump_directory_explicit = false;
  long run_seconds = 0;
  std::string dump_directory = default_dump_directory();
  std::size_t capacity = 8192;
  std::string open_dump;
};

std::string url_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

void print_usage() {
  std::printf(
      "Blackbox — causal flight recorder\n"
      "Usage: blackbox [options]\n"
      "  --no-browser       do not open the system browser\n"
      "  --run-seconds N    stop after N seconds (0 = until signal)\n"
      "  --dump-dir PATH    directory for dumps\n"
      "  --open-dump PATH   open a .bbx dump in the workbench on launch\n"
      "  --no-demo          do not generate demo traffic\n"
      "  --capacity N       records to retain (8..131072)\n"
      "  --help             show this help\n");
}

bool parse_options(int argc, char** argv, AppOptions& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--no-browser") {
      options.no_browser = true;
    } else if (argument == "--no-demo") {
      options.no_demo = true;
    } else if (argument == "--open-dump") {
      if (++i >= argc || argv[i][0] == '\0') {
        return false;
      }
      options.open_dump = argv[i];
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      std::exit(0);
    } else if (argument == "--run-seconds") {
      if (++i >= argc) {
        return false;
      }
      std::uint64_t value = 0;
      if (!parse_u64(argv[i], value) || value > 86'400) {
        return false;
      }
      options.run_seconds = static_cast<long>(value);
    } else if (argument == "--dump-dir") {
      if (++i >= argc || argv[i][0] == '\0') {
        return false;
      }
      options.dump_directory = argv[i];
      options.dump_directory_explicit = true;
    } else if (argument == "--capacity") {
      if (++i >= argc) {
        return false;
      }
      std::uint64_t value = 0;
      if (!parse_u64(argv[i], value) || value < Ring::kMinCapacity ||
          value > Ring::kMaxCapacity) {
        return false;
      }
      options.capacity = static_cast<std::size_t>(value);
    } else {
      std::fprintf(stderr, "Blackbox: unknown option %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

}  // namespace

DemoService::DemoService(Recorder& recorder, DemoConfig config)
    : recorder_(recorder), config_(config) {
  config_.interval_ms = std::clamp(config_.interval_ms, 1, 600'000);
}

DemoService::~DemoService() { stop(); }

void DemoService::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return;
  }
  triggered_.store(true, std::memory_order_release);
  try {
    thread_ = std::thread([this] { run(); });
  } catch (...) {
    running_.store(false, std::memory_order_release);
    throw;
  }
}

void DemoService::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void DemoService::trigger() {
  triggered_.store(true, std::memory_order_release);
  condition_.notify_all();
}

void DemoService::run() {
  while (running_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait_for(lock,
                          std::chrono::milliseconds(config_.interval_ms), [this] {
                            return !running_.load(std::memory_order_acquire) ||
                                   triggered_.load(std::memory_order_acquire);
                          });
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }
      triggered_.store(false, std::memory_order_release);
    }
    generate();
    const std::uint64_t completed =
        scenarios_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (config_.auto_dump_every > 0 &&
        completed % config_.auto_dump_every == 0) {
      (void)recorder_.flush_dump();
    }
  }
}

void DemoService::generate() {
  recorder_.reset_thread_context();
  recorder_.set_correlation(new_correlation_id());
  const std::uint64_t index = next_index_.fetch_add(1, std::memory_order_relaxed);
  const std::int64_t started = now_mono_ns();

  Span request = recorder_.span(
      "http.request", {{"http.route", Value::string("/api/orders")},
                       {"http.method", Value::string("GET")},
                       {"request.index", Value::integer(
                                             static_cast<std::int64_t>(index))}});
  if (!request) {
    return;
  }
  (void)recorder_.log(bb::Level::Info, "request accepted",
                      {{"request.index",
                        Value::integer(static_cast<std::int64_t>(index))}});

  const bool breaker_open = open_breaker_;
  if (!breaker_open && index % 4 == 0) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
      Span query = recorder_.span(
          "db.query", {{"db.system", Value::string("postgres")},
                       {"db.statement", Value::string("SELECT * FROM orders")},
                       {"db.attempt", Value::integer(attempt)}});
      if (query) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8 + attempt));
        (void)recorder_.error(
            "database query timed out", Status::Timeout,
            {{"db.timeout_ms", Value::integer(120)},
             {"db.attempt", Value::integer(attempt)}});
        query.set_status(Status::Timeout);
      }
      if (attempt == 1) {
        (void)recorder_.log(bb::Level::Warn, "retrying database query",
                            {{"retry.delay_ms", Value::integer(25)}});
      }
    }
    (void)recorder_.state_change(
        "breaker.state", Value::string("closed"), Value::string("open"),
        Level::Warn, {{"breaker.failures", Value::integer(2)}});
    open_breaker_ = true;
    open_requests_ = 0;
  }

  if (open_breaker_) {
    (void)recorder_.log(bb::Level::Warn, "circuit breaker is open",
                        {{"breaker.state", Value::string("open")}});
    Span fallback =
        recorder_.span("fallback.cache", {{"cache.key", Value::string("orders")}});
    if (fallback) {
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      (void)recorder_.log(bb::Level::Info, "serving cached response",
                          {{"cache.age_s", Value::integer(4)}});
      (void)recorder_.metric("cache.hits", std::int64_t{1},
                             {{"cache.key", Value::string("orders")}});
      fallback.set_status(Status::Ok);
    }
    ++open_requests_;
    if (open_requests_ >= 2) {
      (void)recorder_.state_change(
          "breaker.state", Value::string("open"), Value::string("closed"),
          Level::Info, {{"breaker.cooldown_ms", Value::integer(2000)}});
      open_breaker_ = false;
      open_requests_ = 0;
    }
  } else {
    Span query = recorder_.span(
        "db.query", {{"db.system", Value::string("postgres")},
                     {"db.statement", Value::string("SELECT * FROM orders")}});
    if (query) {
      std::this_thread::sleep_for(std::chrono::milliseconds(6));
      (void)recorder_.log(bb::Level::Info, "query returned rows",
                          {{"db.rows", Value::integer(42)}});
      (void)recorder_.metric("db.rows", std::int64_t{42},
                             {{"db.system", Value::string("postgres")}});
      query.set_status(Status::Ok);
    }
  }

  if (!open_breaker_) {
    (void)recorder_.metric("request.count", std::int64_t{1},
                           {{"http.route", Value::string("/api/orders")}});
  }
  const double elapsed_ms =
      static_cast<double>(now_mono_ns() - started) / 1'000'000.0;
  (void)recorder_.metric("request.duration_ms", elapsed_ms,
                         {{"http.route", Value::string("/api/orders")}});
  (void)recorder_.log(bb::Level::Info, "request completed",
                      {{"http.route", Value::string("/api/orders")},
                       {"breaker.open", Value::boolean(open_breaker_)}});
  request.set_status(Status::Ok);
}

int run_demo_app(int argc, char** argv) {
  AppOptions options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }
  g_stop = 0;
  std::signal(SIGINT, stop_signal);
  std::signal(SIGTERM, stop_signal);

  std::string initial_dump_name;
  if (!options.open_dump.empty()) {
    const std::filesystem::path dump_path(options.open_dump);
    if (dump_path.extension() != ".bbx") {
      std::fprintf(stderr, "Blackbox: --open-dump requires a .bbx file\n");
      return 2;
    }
    DumpMetadata metadata;
    std::string metadata_error;
    if (!read_dump_metadata(dump_path.string(), metadata, &metadata_error)) {
      std::fprintf(stderr, "Blackbox: cannot open dump: %s\n",
                   metadata_error.c_str());
      return 2;
    }
    initial_dump_name = dump_path.filename().string();
    if (!options.dump_directory_explicit) {
      const auto parent = dump_path.parent_path();
      options.dump_directory = parent.empty() ? std::string(".") : parent.string();
    }
  }

  RecorderConfig config;
  config.ring_capacity = options.capacity;
  config.dump_directory = options.dump_directory;
  config.install_crash_handlers = true;
  Recorder recorder(config);
  DemoService demo(recorder, DemoConfig{1200, 12});
  std::function<void()> trigger;
  if (!options.no_demo) {
    trigger = [&demo] { demo.trigger(); };
  }
  UiServer server(recorder, trigger);

  std::string error;
  if (!server.start(&error)) {
    std::fprintf(stderr, "Blackbox: cannot start UI server: %s\n",
                 error.c_str());
    demo.stop();
    return 1;
  }
  if (!options.no_demo) {
    demo.start();
  }
  std::string url = server.url();
  if (!initial_dump_name.empty()) {
    url += "&dump=" + url_encode(initial_dump_name);
  }
  std::printf("%s\n%s\n", build_info().c_str(), url.c_str());
  std::fflush(stdout);
  if (!options.no_browser) {
    std::string browser_error;
    if (!open_browser(url, &browser_error)) {
      std::fprintf(stderr, "Blackbox: %s\nOpen this URL in a browser: %s\n",
                   browser_error.c_str(), url.c_str());
    }
  }

  const auto started = std::chrono::steady_clock::now();
  while (g_stop == 0) {
    if (!server.running()) {
      break;
    }
    if (options.run_seconds > 0) {
      const auto elapsed = std::chrono::steady_clock::now() - started;
      if (elapsed >= std::chrono::seconds(options.run_seconds)) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  demo.stop();
  server.stop();
  std::printf("Blackbox stopped.\n");
  return 0;
}

}  // namespace bb
