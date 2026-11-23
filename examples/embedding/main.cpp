// Headless embedder: record a small failure, analyse it, write a dump and read
// it back. No browser and no server required.
#include <blackbox/causal.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/query.hpp>
#include <blackbox/recorder.hpp>
#include <blackbox/util/random.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

int main() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("blackbox-embedding-" + bb::random_token(6));
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::fprintf(stderr, "cannot create directory\n");
    return 1;
  }

  int status = 0;
  {
    bb::RecorderConfig config;
    config.ring_capacity = 512;
    config.dump_directory = directory.string();
    config.install_crash_handlers = false;
    bb::Recorder recorder(config);

    recorder.reset_thread_context();
    recorder.set_correlation(bb::new_correlation_id());
    {
      bb::Span request = recorder.span(
          "http.request", {{"http.route", bb::Value::string("/orders")}});
      {
        bb::Span query = recorder.span(
            "db.query", {{"db.system", bb::Value::string("postgres")}});
        recorder.error("database timeout", bb::Status::Timeout,
                       {{"db.timeout_ms", bb::Value::integer(120)}});
        query.set_status(bb::Status::Timeout);
      }
      recorder.state_change("breaker.state", bb::Value::string("closed"),
                            bb::Value::string("open"), bb::Level::Warn);
      {
        bb::Span fallback = recorder.span("fallback.cache");
        recorder.log(bb::Level::Info, "served from cache");
        fallback.set_status(bb::Status::Ok);
      }
      request.set_status(bb::Status::Ok);
    }
    recorder.metric("request.duration_ms", 12.5);

    const bb::RecorderSnapshot snapshot = recorder.snapshot();
    std::string graph_error;
    const bb::CausalGraph graph =
        bb::build_causal_graph(snapshot.records, &graph_error);
    const bb::FailureExplanation failure = bb::explain_latest_failure(graph);
    const auto query = bb::Query::parse("kind = error");
    const auto errors = query ? bb::filter_records(snapshot.records, *query)
                              : std::vector<bb::Record>{};

    std::printf("records: %zu\n", snapshot.records.size());
    std::printf("graph nodes: %zu, edges: %zu\n", graph.size(), graph.edges.size());
    std::printf("failure found: %s\n", failure.found ? "yes" : "no");
    std::printf("root cause: %s\n", failure.summary.c_str());
    std::printf("error records: %zu\n", errors.size());

    const bb::DumpResult dump = recorder.flush_dump();
    if (!dump.ok) {
      std::fprintf(stderr, "dump failed: %s\n", dump.error.c_str());
      status = 1;
    } else {
      const bb::DumpReadResult read = bb::read_dump_file(dump.path);
      std::printf("dump: %s (%zu records, complete=%s)\n", dump.path.c_str(),
                  read.records.size(), read.complete ? "yes" : "no");
      if (!read.ok || !read.complete ||
          read.records.size() != snapshot.records.size() ||
          !failure.found || errors.empty()) {
        status = 1;
      }
    }
  }
  std::filesystem::remove_all(directory, error);
  return status;
}
