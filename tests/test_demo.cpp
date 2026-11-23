#include "harness.hpp"
#include "support.hpp"

#include <blackbox/causal.hpp>
#include <blackbox/demo.hpp>
#include <blackbox/recorder.hpp>

#include <chrono>
#include <set>
#include <string>
#include <thread>

namespace {

bool wait_for_scenarios(const bb::DemoService& demo, std::uint64_t count) {
  for (int i = 0; i < 300; ++i) {
    if (demo.scenarios() >= count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

BB_TEST(demo_generates_failure_and_recovery_chain) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 4096;
  config.dump_directory = temp.file("dumps");
  bb::Recorder recorder(config);
  {
    bb::DemoService demo(recorder, bb::DemoConfig{5, 4});
    demo.start();
    CHECK(wait_for_scenarios(demo, 4));
    demo.stop();
    CHECK(!demo.running());
  }
  CHECK(recorder.crash_handler_active());

  const auto snapshot = recorder.snapshot(4000);
  CHECK(snapshot.records.size() > 30);
  bool saw_request = false;
  bool saw_db = false;
  bool saw_fallback = false;
  bool saw_breaker = false;
  std::set<bb::CorrelationId> correlations;
  std::vector<bb::Record> records = snapshot.records;
  for (const auto& record : records) {
    if (record.kind == bb::RecordKind::SpanBegin &&
        bb::record_name(record) == "http.request") {
      saw_request = true;
      correlations.insert(record.correlation);
    }
    if (record.kind == bb::RecordKind::SpanBegin &&
        bb::record_name(record) == "db.query") {
      saw_db = true;
    }
    if (record.kind == bb::RecordKind::SpanBegin &&
        bb::record_name(record) == "fallback.cache") {
      saw_fallback = true;
    }
    if (record.kind == bb::RecordKind::StateChange &&
        bb::record_state_key(record) == "breaker.state") {
      saw_breaker = true;
    }
  }
  CHECK(saw_request);
  CHECK(saw_db);
  CHECK(saw_fallback);
  CHECK(saw_breaker);
  CHECK(correlations.size() >= 2);

  std::string error;
  const auto graph = bb::build_causal_graph(records, &error);
  CHECK(error.empty());
  const auto explanation = bb::explain_latest_failure(graph);
  CHECK(explanation.found);
  CHECK(!explanation.chain.empty());
  CHECK(!explanation.findings.empty());
  CHECK(explanation.summary.find("root cause") != std::string::npos);
  const auto waterfall = bb::span_waterfall(graph);
  CHECK(!waterfall.empty());

  const auto dumps = recorder.list_dumps();
  CHECK(!dumps.empty());
  CHECK(dumps.front().valid);
}

BB_TEST(demo_trigger_runs_immediately) {
  bb::Recorder recorder(bb::RecorderConfig{256});
  bb::DemoService demo(recorder, bb::DemoConfig{60000, 0});
  demo.start();
  demo.trigger();
  CHECK(wait_for_scenarios(demo, 1));
  demo.trigger();
  CHECK(wait_for_scenarios(demo, 2));
  demo.stop();
  CHECK(recorder.snapshot().records.size() > 5);
}
