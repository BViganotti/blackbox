// Short soak: run the demo and concurrently snapshot, analyse, query and flush
// for a bounded number of seconds, asserting the same invariants the services
// rely on. This is not a substitute for an hours-long production soak, but it
// catches growth, deadlocks and retention mistakes in a CI-sized window.
#include <blackbox/causal.hpp>
#include <blackbox/demo.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/query.hpp>
#include <blackbox/recorder.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

bool valid_graph(const bb::CausalGraph& graph) {
  for (const auto& edge : graph.edges) {
    if (edge.from >= edge.to || edge.to >= graph.records.size()) {
      return false;
    }
    if (graph.records[edge.from].seq >= graph.records[edge.to].seq) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = 10;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
      if (seconds < 1 || seconds > 3600) {
        std::printf("--seconds must be between 1 and 3600\n");
        return 2;
      }
    } else {
      std::printf("usage: blackbox_soak [--seconds N]\n");
      return 2;
    }
  }

  std::error_code error;
  const auto directory =
      std::filesystem::temp_directory_path(error) /
      ("blackbox-soak-" + bb::random_token(6));
  if (error) {
    std::printf("cannot create soak temp path\n");
    return 1;
  }
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::printf("cannot create soak directory\n");
    return 1;
  }

  int status = 0;
  {
    bb::RecorderConfig config;
    config.ring_capacity = 512;
    config.dump_directory = directory.string();
    config.max_dumps = 4;
    config.max_dump_bytes = 64u * 1024u * 1024u;
    config.install_crash_handlers = false;
    bb::Recorder recorder(config);
    bb::DemoService demo(recorder, bb::DemoConfig{5, 0});
    demo.start();

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t last_produced = 0;
    std::uint32_t analyses = 0;
    std::uint32_t flushes = 0;
    std::uint64_t analysed_records = 0;
    const auto query = bb::Query::parse("level >= warn");
    while (std::chrono::steady_clock::now() - started <
           std::chrono::seconds(seconds)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const auto snapshot = recorder.snapshot(512);
      if (snapshot.records.empty()) {
        continue;
      }
      if (snapshot.ring.produced < last_produced) {
        std::printf("produced sequence regressed\n");
        status = 1;
        break;
      }
      last_produced = snapshot.ring.produced;
      for (const auto& record : snapshot.records) {
        if (!bb::validate_record(record)) {
          std::printf("invalid record in snapshot\n");
          status = 1;
          break;
        }
      }
      if (status != 0) {
        break;
      }
      std::string graph_error;
      const auto graph = bb::build_causal_graph(snapshot.records, &graph_error);
      if (!graph_error.empty() || !valid_graph(graph)) {
        std::printf("causal invariant failed: %s\n", graph_error.c_str());
        status = 1;
        break;
      }
      if (query.has_value()) {
        (void)bb::filter_records(snapshot.records, *query);
      }
      if (analyses % 20 == 0) {
        const auto explanation = bb::explain_latest_failure(graph);
        if (explanation.found && explanation.chain.empty()) {
          std::printf("explanation has no chain\n");
          status = 1;
          break;
        }
      }
      analysed_records += snapshot.records.size();
      ++analyses;
      if (analyses % 20 == 0) {
        const auto dump = recorder.flush_dump();
        if (!dump.ok) {
          std::printf("flush failed: %s\n", dump.error.c_str());
          status = 1;
          break;
        }
        const auto dumps = recorder.list_dumps(16);
        if (dumps.size() > 4) {
          std::printf("retention kept %zu dumps\n", dumps.size());
          status = 1;
          break;
        }
        if (!dumps.empty() && dumps.front().valid) {
          bb::DumpReadOptions options;
          options.require_footer = false;
          const auto read = bb::read_dump_file(dumps.front().path, options);
          if (!read.ok || read.records.empty()) {
            std::printf("soak dump unreadable: %s\n", read.error.c_str());
            status = 1;
            break;
          }
        }
        ++flushes;
      }
    }
    demo.stop();
    const auto stats = recorder.stats();
    std::printf(
        "soak: %d s, %llu scenarios, %llu produced, %u analyses, %u flushes, "
        "%llu record copies\n",
        seconds, static_cast<unsigned long long>(demo.scenarios()),
        static_cast<unsigned long long>(stats.ring.produced), analyses, flushes,
        static_cast<unsigned long long>(analysed_records));
    if (demo.scenarios() == 0) {
      std::printf("soak produced no scenarios\n");
      status = 1;
    }
  }
  std::filesystem::remove_all(directory, error);
  if (status == 0) {
    std::printf("OK: soak invariants held\n");
  }
  return status;
}
