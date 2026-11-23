#include "harness.hpp"
#include "support.hpp"

#include <blackbox/causal.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

bb::Record base_record(std::uint64_t seq, bb::TraceId trace) {
  bb::Record record{};
  record.seq = seq;
  record.trace = trace;
  record.correlation = 0xC0FFEE;
  record.wall_ns = 1'700'000'000'000'000'000ll + static_cast<std::int64_t>(seq);
  record.mono_ns = static_cast<std::int64_t>(seq) * 1'000'000;
  record.cause_seq = seq > 1 ? seq - 1 : 0;
  return record;
}

std::vector<bb::Record> failure_trace() {
  const bb::TraceId trace{0xAA, 0xBB};
  std::vector<bb::Record> records;
  bb::Record log = base_record(1, trace);
  log.kind = bb::RecordKind::Log;
  log.level = bb::Level::Info;
  bb::set_record_message(log, "request received");
  records.push_back(log);

  bb::Record begin = base_record(2, trace);
  begin.kind = bb::RecordKind::SpanBegin;
  begin.span = 10;
  bb::set_record_name(begin, "db.query");
  records.push_back(begin);

  bb::Record error = base_record(3, trace);
  error.kind = bb::RecordKind::Error;
  error.level = bb::Level::Error;
  error.status = bb::Status::Timeout;
  error.span = 10;
  bb::set_record_message(error, "database timeout");
  records.push_back(error);

  bb::Record end = base_record(4, trace);
  end.kind = bb::RecordKind::SpanEnd;
  end.span = 10;
  end.status = bb::Status::Timeout;
  end.level = bb::Level::Error;
  end.duration_ns = 5'000'000;
  bb::set_record_name(end, "db.query");
  records.push_back(end);

  bb::Record state = base_record(5, trace);
  state.kind = bb::RecordKind::StateChange;
  state.level = bb::Level::Warn;
  bb::set_record_state_key(state, "breaker.state");
  (void)bb::store_value(bb::Value::string("closed"), state.state_old);
  (void)bb::store_value(bb::Value::string("open"), state.state_new);
  bb::set_record_message(state, "too many failures");
  records.push_back(state);

  bb::Record fallback_begin = base_record(6, trace);
  fallback_begin.kind = bb::RecordKind::SpanBegin;
  fallback_begin.span = 11;
  bb::set_record_name(fallback_begin, "fallback");
  records.push_back(fallback_begin);

  bb::Record fallback_end = base_record(7, trace);
  fallback_end.kind = bb::RecordKind::SpanEnd;
  fallback_end.span = 11;
  fallback_end.status = bb::Status::Ok;
  fallback_end.duration_ns = 1'000'000;
  bb::set_record_name(fallback_end, "fallback");
  records.push_back(fallback_end);

  bb::Record served = base_record(8, trace);
  served.kind = bb::RecordKind::Log;
  served.level = bb::Level::Info;
  bb::set_record_message(served, "served from cache");
  records.push_back(served);
  return records;
}

}  // namespace

BB_TEST(causal_graph_builds_forward_edges_and_roots) {
  const auto records = failure_trace();
  std::string error;
  const bb::CausalGraph graph = bb::build_causal_graph(records, &error);
  CHECK(error.empty());
  CHECK_EQ(graph.size(), records.size());
  CHECK_EQ(graph.roots.size(), 1u);
  CHECK_EQ(graph.roots[0], 0u);
  CHECK(graph.edges.size() >= 8);
  for (const auto& edge : graph.edges) {
    CHECK(edge.from < edge.to);
    CHECK(graph.records[edge.from].seq < graph.records[edge.to].seq);
  }
  const auto node = graph.find_seq(3);
  CHECK(node.has_value());
  CHECK_EQ(*node, 2u);
  CHECK(!graph.find_seq(999).has_value());
  CHECK_EQ(graph.nodes_for_correlation(0xC0FFEE).size(), records.size());
  CHECK_EQ(graph.records[0].seq, 1u);
}

BB_TEST(causal_graph_rejects_malformed_input) {
  std::string error;
  auto duplicate = failure_trace();
  duplicate[1].seq = duplicate[0].seq;
  auto graph = bb::build_causal_graph(duplicate, &error);
  CHECK(graph.empty());
  CHECK(!error.empty());

  error.clear();
  std::vector<bb::Record> invalid(1);
  invalid[0].kind = static_cast<bb::RecordKind>(99);
  graph = bb::build_causal_graph(invalid, &error);
  CHECK(graph.empty());
  CHECK(!error.empty());

  error.clear();
  auto zero = failure_trace();
  zero[0].seq = 0;
  graph = bb::build_causal_graph(zero, &error);
  CHECK(graph.empty());
  CHECK(!error.empty());
}

BB_TEST(causal_graph_counts_unresolved_parents) {
  const bb::TraceId trace{1, 2};
  bb::Record begin = base_record(1, trace);
  begin.kind = bb::RecordKind::SpanBegin;
  begin.span = 5;
  begin.parent = 4;
  bb::set_record_name(begin, "orphan");
  std::string error;
  const auto graph = bb::build_causal_graph({begin}, &error);
  CHECK(error.empty());
  CHECK_EQ(graph.unresolved_parents, 1u);
  CHECK_EQ(graph.roots.size(), 1u);
}

BB_TEST(causal_graph_links_parent_spans) {
  const bb::TraceId trace{7, 8};
  bb::Record outer = base_record(1, trace);
  outer.kind = bb::RecordKind::SpanBegin;
  outer.span = 100;
  bb::set_record_name(outer, "outer");
  bb::Record inner = base_record(2, trace);
  inner.kind = bb::RecordKind::SpanBegin;
  inner.span = 101;
  inner.parent = 100;
  bb::set_record_name(inner, "inner");
  std::string error;
  const auto graph = bb::build_causal_graph({outer, inner}, &error);
  CHECK(error.empty());
  CHECK_EQ(graph.unresolved_parents, 0u);
  bool found_parent = false;
  for (const auto& edge : graph.edges) {
    if (edge.kind == bb::CausalEdge::Kind::Parent) {
      CHECK_EQ(edge.from, 0u);
      CHECK_EQ(edge.to, 1u);
      found_parent = true;
    }
  }
  CHECK(found_parent);
}

BB_TEST(causal_layout_is_deterministic_and_layered) {
  const auto records = failure_trace();
  std::string error;
  const auto graph = bb::build_causal_graph(records, &error);
  const auto first = bb::layout_causal_graph(graph);
  const auto second = bb::layout_causal_graph(graph);
  CHECK_EQ(first.points.size(), graph.size());
  CHECK_EQ(first.layers, second.layers);
  CHECK(first.layers >= 2);
  for (std::size_t i = 0; i < first.points.size(); ++i) {
    CHECK_EQ(first.points[i].layer, second.points[i].layer);
    CHECK_EQ(first.points[i].row, second.points[i].row);
    CHECK_NEAR(first.points[i].x, static_cast<float>(first.points[i].layer) * 220.0f, 0.01);
    CHECK_NEAR(first.points[i].y, static_cast<float>(first.points[i].row) * 64.0f + 40.0f, 0.01);
  }
  CHECK(first.widest_layer >= 1);
}

BB_TEST(causal_span_waterfall) {
  const auto records = failure_trace();
  std::string error;
  const auto graph = bb::build_causal_graph(records, &error);
  const auto rows = bb::span_waterfall(graph);
  CHECK_EQ(rows.size(), 2u);
  CHECK_EQ(rows[0].name, std::string("db.query"));
  CHECK_EQ(static_cast<int>(rows[0].status), static_cast<int>(bb::Status::Timeout));
  CHECK_EQ(rows[0].duration_ns, 5'000'000);
  CHECK(rows[0].end_node != bb::kInvalidNode);
  CHECK_EQ(rows[1].name, std::string("fallback"));
  CHECK_EQ(rows[1].duration_ns, 1'000'000);
}

BB_TEST(causal_explain_failure_from_error) {
  const auto records = failure_trace();
  std::string error;
  const auto graph = bb::build_causal_graph(records, &error);
  const bb::FailureExplanation explanation = bb::explain_failure(graph, 3);
  CHECK(explanation.found);
  CHECK_EQ(explanation.error_node, 3u);
  CHECK_EQ(explanation.root_cause, 2u);
  CHECK_EQ(explanation.chain.front(), 0u);
  CHECK_EQ(explanation.chain.back(), 3u);
  CHECK_EQ(explanation.findings.size(), explanation.chain.size());
  CHECK(explanation.summary.find("database timeout") != std::string::npos);
}

BB_TEST(causal_explain_latest_failure_prefers_breaker_state) {
  const auto records = failure_trace();
  std::string error;
  const auto graph = bb::build_causal_graph(records, &error);
  const bb::FailureExplanation explanation = bb::explain_latest_failure(graph);
  CHECK(explanation.found);
  CHECK_EQ(explanation.error_node, 4u);
  CHECK_EQ(explanation.root_cause, 2u);
  CHECK(explanation.summary.find("root cause") != std::string::npos);
  CHECK(!explanation.findings.empty());
}

BB_TEST(causal_explain_handles_missing_failure) {
  const bb::TraceId trace{1, 1};
  bb::Record log = base_record(1, trace);
  log.kind = bb::RecordKind::Log;
  bb::set_record_message(log, "all good");
  std::string error;
  const auto graph = bb::build_causal_graph({log}, &error);
  const auto explanation = bb::explain_latest_failure(graph);
  CHECK(!explanation.found);
  CHECK(explanation.chain.empty());
  const auto invalid = bb::explain_failure(graph, 99);
  CHECK(!invalid.found);
}

BB_TEST(causal_state_timeline_groups_transitions) {
  const bb::TraceId trace{4, 4};
  bb::Record first = base_record(1, trace);
  first.kind = bb::RecordKind::StateChange;
  bb::set_record_state_key(first, "breaker.state");
  (void)bb::store_value(bb::Value::string("closed"), first.state_old);
  (void)bb::store_value(bb::Value::string("open"), first.state_new);
  bb::Record second = base_record(2, trace);
  second.kind = bb::RecordKind::StateChange;
  bb::set_record_state_key(second, "breaker.state");
  (void)bb::store_value(bb::Value::string("open"), second.state_old);
  (void)bb::store_value(bb::Value::string("closed"), second.state_new);
  bb::Record other = base_record(3, trace);
  other.kind = bb::RecordKind::StateChange;
  bb::set_record_state_key(other, "worker.state");
  (void)bb::store_value(bb::Value::string("idle"), other.state_old);
  (void)bb::store_value(bb::Value::string("busy"), other.state_new);
  std::string error;
  const auto graph = bb::build_causal_graph({first, second, other}, &error);
  const auto histories = bb::state_timeline(graph);
  CHECK_EQ(histories.size(), 2u);
  CHECK_EQ(histories[0].key, std::string("breaker.state"));
  CHECK_EQ(histories[0].transitions.size(), 2u);
  CHECK_EQ(histories[0].transitions[0].next.as_string(), std::string("open"));
  CHECK_EQ(histories[0].transitions[1].previous.as_string(), std::string("open"));
  CHECK_EQ(histories[1].key, std::string("worker.state"));
  CHECK_EQ(histories[1].transitions.size(), 1u);
  const auto limited = bb::state_timeline(graph, 1, 10);
  CHECK_EQ(limited.size(), 1u);
  const auto no_transitions = bb::state_timeline(graph, 10, 0);
  CHECK(!no_transitions.empty());
  CHECK(no_transitions[0].transitions.empty());
}

BB_TEST(causal_compare_traces_detects_first_event_difference) {
  const auto records = failure_trace();
  std::string error;
  const auto left = bb::build_causal_graph(records, &error);
  auto mutated_records = records;
  mutated_records[3].status = bb::Status::Ok;
  const auto right = bb::build_causal_graph(mutated_records, &error);
  const auto same = bb::compare_traces(left, left);
  CHECK(!same.event_divergence);
  CHECK(!same.state_divergence);
  const auto different = bb::compare_traces(left, right);
  CHECK(different.event_divergence);
  CHECK_EQ(different.event_index, 3u);
  CHECK(different.event_detail.find("status") != std::string::npos);
}

BB_TEST(causal_compare_traces_detects_length_difference) {
  auto records = failure_trace();
  std::string error;
  const auto full = bb::build_causal_graph(records, &error);
  records.pop_back();
  const auto short_trace = bb::build_causal_graph(records, &error);
  const auto result = bb::compare_traces(full, short_trace);
  CHECK(result.event_divergence);
  CHECK_EQ(result.event_index, 7u);
  CHECK_EQ(result.right_event, bb::kInvalidNode);
}

BB_TEST(causal_compare_traces_detects_state_divergence) {
  const bb::TraceId trace{9, 9};
  auto make = [&](const std::string& next) {
    std::vector<bb::Record> records;
    bb::Record change = base_record(1, trace);
    change.kind = bb::RecordKind::StateChange;
    bb::set_record_state_key(change, "mode");
    (void)bb::store_value(bb::Value::string("idle"), change.state_old);
    (void)bb::store_value(bb::Value::string(next), change.state_new);
    records.push_back(change);
    return records;
  };
  std::string error;
  const auto left = bb::build_causal_graph(make("busy"), &error);
  const auto right = bb::build_causal_graph(make("failed"), &error);
  const auto result = bb::compare_traces(left, right);
  CHECK(result.event_divergence);
  CHECK(result.state_divergence);
  CHECK_EQ(result.state_key, std::string("mode"));
  CHECK_EQ(result.state_index, 0u);
  CHECK_EQ(result.left_value.as_string(), std::string("busy"));
  CHECK_EQ(result.right_value.as_string(), std::string("failed"));
}

BB_TEST(causal_randomized_graphs_remain_acyclic) {
  bb::Xoshiro128 random(0xCA05A1);
  for (int round = 0; round < 20; ++round) {
    const std::uint64_t count = 1 + random.bounded(120);
    const bb::TraceId trace{0xDEAD0000ull + round, 0xBEEF0000ull + round};
    std::vector<bb::Record> records;
    records.reserve(count);
    for (std::uint64_t i = 1; i <= count; ++i) {
      bb::Record record = base_record(i, trace);
      record.span = i;
      record.cause_seq = i > 1 ? 1 + random.bounded(static_cast<std::uint32_t>(i - 1)) : 0;
      if (i % 10 == 1) {
        record.kind = bb::RecordKind::SpanBegin;
        (void)bb::set_record_name(record, "random.span");
      } else if (i % 10 == 6 && i > 5) {
        record.kind = bb::RecordKind::SpanEnd;
        record.span = i - 5;
        record.status = bb::Status::Ok;
        (void)bb::set_record_name(record, "random.span");
      } else if (i % 17 == 0) {
        record.kind = bb::RecordKind::Error;
        record.level = bb::Level::Error;
        record.status = bb::Status::Timeout;
        (void)bb::set_record_message(record, "random failure");
      } else {
        record.kind = bb::RecordKind::Log;
        record.level = bb::Level::Info;
        (void)bb::set_record_message(record, "random log");
      }
      CHECK(bb::validate_record(record));
      records.push_back(record);
    }
    for (std::size_t i = records.size(); i > 1; --i) {
      const std::size_t j = random.bounded(static_cast<std::uint32_t>(i));
      std::swap(records[i - 1], records[j]);
    }
    std::string error;
    const auto graph = bb::build_causal_graph(records, &error);
    CHECK(error.empty());
    CHECK_EQ(graph.size(), records.size());
    for (const auto& edge : graph.edges) {
      CHECK(edge.from < edge.to);
      CHECK(graph.records[edge.from].seq < graph.records[edge.to].seq);
      CHECK(edge.kind == bb::CausalEdge::Kind::Cause ||
            edge.kind == bb::CausalEdge::Kind::Parent ||
            edge.kind == bb::CausalEdge::Kind::Span ||
            edge.kind == bb::CausalEdge::Kind::Correlation);
    }
    const auto layout = bb::layout_causal_graph(graph);
    CHECK_EQ(layout.points.size(), graph.size());
    for (std::size_t i = 0; i < layout.points.size(); ++i) {
      CHECK(layout.points[i].layer <= layout.layers);
      CHECK(layout.points[i].row < layout.widest_layer);
    }
    const auto explanation = bb::explain_latest_failure(graph);
    if (explanation.found) {
      CHECK(!explanation.chain.empty());
      CHECK(explanation.chain.size() <= graph.size());
      CHECK_EQ(explanation.findings.size(), explanation.chain.size());
    }
    const auto same = bb::compare_traces(graph, graph);
    CHECK(!same.event_divergence);
    CHECK(!same.state_divergence);
  }
}
