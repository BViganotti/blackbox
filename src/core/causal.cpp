#include <blackbox/causal.hpp>

#include <blackbox/util/time.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace bb {
namespace {

constexpr std::size_t kMaxCausalRecords = 100'000;
constexpr std::size_t kMaxExplainSteps = 256;

struct SpanKey {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  SpanId span = kNoSpan;
  bool operator==(const SpanKey& other) const noexcept {
    return high == other.high && low == other.low && span == other.span;
  }
};

struct SpanKeyHash {
  std::size_t operator()(const SpanKey& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.high);
    const std::size_t low_hash = std::hash<std::uint64_t>{}(key.low);
    hash ^= low_hash + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    hash ^= std::hash<std::uint64_t>{}(key.span) + 0x9E3779B97F4A7C15ull +
            (hash << 6) + (hash >> 2);
    return hash;
  }
};

SpanKey span_key(const Record& record) {
  return SpanKey{record.trace.high, record.trace.low, record.span};
}

int edge_priority(CausalEdge::Kind kind) noexcept {
  switch (kind) {
    case CausalEdge::Kind::Cause:
      return 0;
    case CausalEdge::Kind::Span:
      return 1;
    case CausalEdge::Kind::Parent:
      return 2;
    case CausalEdge::Kind::Correlation:
      return 3;
  }
  return 4;
}

bool value_is_abnormal(const Value& value) {
  if (value.type() != ValueType::String) {
    return false;
  }
  const std::string& text = value.as_string();
  return text == "open" || text == "error" || text == "failed" ||
         text == "tripped" || text == "true" || text == "unhealthy";
}

bool record_is_abnormal(const Record& record) {
  if (record.kind == RecordKind::Error) {
    return true;
  }
  if (record.level == Level::Error || record.level == Level::Fatal) {
    return true;
  }
  if (record.status == Status::Error || record.status == Status::Timeout) {
    return true;
  }
  if (record.kind == RecordKind::StateChange &&
      value_is_abnormal(owned_value(record.state_new))) {
    return true;
  }
  return false;
}

std::string node_description(const Record& record) {
  switch (record.kind) {
    case RecordKind::SpanBegin:
      return "span '" + std::string(record_name(record)) + "' started";
    case RecordKind::SpanEnd: {
      std::string text = "span '" + std::string(record_name(record)) +
                         "' ended " + to_string(record.status);
      if (record.duration_ns > 0) {
        text += " after " + format_duration_ns(record.duration_ns);
      }
      return text;
    }
    case RecordKind::StateChange:
      return "state '" + std::string(record_state_key(record)) + "' " +
             owned_value(record.state_old).display() + " -> " +
             owned_value(record.state_new).display();
    case RecordKind::Error:
      return "error (" + std::string(to_string(record.status)) + "): " +
             std::string(record_message(record));
    case RecordKind::Metric:
      return "metric '" + std::string(record_name(record)) + "' = " +
             owned_value(record.value).display();
    case RecordKind::Log: {
      std::string text = to_string(record.level);
      text += " log: ";
      text += record_message(record);
      return text;
    }
    case RecordKind::Mark:
      return "mark '" + std::string(record_name(record)) + "'";
    case RecordKind::Note:
      return "note: " + std::string(record_message(record));
  }
  return "record";
}

std::string shape_detail(const Record& left, const Record& right) {
  if (left.kind != right.kind) {
    return std::string("kind differs (") + to_string(left.kind) + " vs " +
           to_string(right.kind) + ")";
  }
  if (left.level != right.level) {
    return std::string("level differs (") + to_string(left.level) + " vs " +
           to_string(right.level) + ")";
  }
  if (left.status != right.status) {
    return std::string("status differs (") + to_string(left.status) + " vs " +
           to_string(right.status) + ")";
  }
  if (record_name(left) != record_name(right)) {
    return "name differs ('" + std::string(record_name(left)) + "' vs '" +
           std::string(record_name(right)) + "')";
  }
  if (record_state_key(left) != record_state_key(right)) {
    return "state key differs";
  }
  const Value left_state = owned_value(left.state_new);
  const Value right_state = owned_value(right.state_new);
  if (!(left_state == right_state)) {
    return "new state differs ('" + left_state.display() + "' vs '" +
           right_state.display() + "')";
  }
  if (record_message(left) != record_message(right)) {
    return "message differs";
  }
  return "record shape differs";
}

std::unordered_map<std::string, const StateHistory*> history_index(
    const std::vector<StateHistory>& histories) {
  std::unordered_map<std::string, const StateHistory*> index;
  for (const StateHistory& history : histories) {
    index.emplace(history.key, &history);
  }
  return index;
}

}  // namespace

const char* to_string(CausalEdge::Kind kind) noexcept {
  switch (kind) {
    case CausalEdge::Kind::Cause:
      return "cause";
    case CausalEdge::Kind::Parent:
      return "parent";
    case CausalEdge::Kind::Span:
      return "span";
    case CausalEdge::Kind::Correlation:
      return "correlation";
  }
  return "unknown";
}

const Record& CausalGraph::record(std::uint32_t node) const {
  if (node >= records.size()) {
    throw std::out_of_range("causal node out of range");
  }
  return records[node];
}

std::optional<std::uint32_t> CausalGraph::find_seq(std::uint64_t seq) const {
  const auto found = by_seq_.find(seq);
  if (found == by_seq_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<std::uint32_t> CausalGraph::nodes_for_correlation(
    CorrelationId id) const {
  const auto found = by_correlation_.find(id);
  if (found == by_correlation_.end()) {
    return {};
  }
  return found->second;
}

CausalGraph build_causal_graph(std::vector<Record> records, std::string* error) {
  CausalGraph graph;
  if (records.size() > kMaxCausalRecords) {
    if (error != nullptr) {
      *error = "too many records for causal analysis";
    }
    return graph;
  }
  for (const Record& record : records) {
    if (!validate_record(record)) {
      if (error != nullptr) {
        *error = "invalid record in causal input";
      }
      return CausalGraph{};
    }
  }
  std::sort(records.begin(), records.end(),
            [](const Record& a, const Record& b) { return a.seq < b.seq; });
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].seq == 0) {
      if (error != nullptr) {
        *error = "zero sequence in causal input";
      }
      return CausalGraph{};
    }
    if (i > 0 && records[i].seq == records[i - 1].seq) {
      if (error != nullptr) {
        *error = "duplicate sequence in causal input";
      }
      return CausalGraph{};
    }
  }
  graph.records = std::move(records);
  graph.by_seq_.reserve(graph.records.size());
  graph.by_correlation_.reserve(graph.records.size() / 4 + 1);
  graph.by_state_key_.reserve(16);

  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    const auto node = static_cast<std::uint32_t>(i);
    const Record& record = graph.records[i];
    graph.by_seq_.emplace(record.seq, node);
    if (record.correlation != kNoCorrelation) {
      graph.by_correlation_[record.correlation].push_back(node);
    }
    if (record.kind == RecordKind::StateChange) {
      graph.by_state_key_[std::string(record_state_key(record))].push_back(node);
    }
  }

  std::unordered_map<SpanKey, std::uint32_t, SpanKeyHash> spans;
  spans.reserve(graph.records.size() / 2 + 1);
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    if (graph.records[i].kind == RecordKind::SpanBegin) {
      spans.emplace(span_key(graph.records[i]), static_cast<std::uint32_t>(i));
    }
  }

  std::vector<CausalEdge> edges;
  edges.reserve(graph.records.size() * 2);
  const auto add_edge = [&graph, &edges](std::uint32_t from, std::uint32_t to,
                                         CausalEdge::Kind kind) {
    if (from == kInvalidNode || to == kInvalidNode || from == to || from >= graph.records.size() ||
        to >= graph.records.size()) {
      return;
    }
    if (graph.records[from].seq >= graph.records[to].seq) {
      return;  // Acyclic by construction: only forward edges.
    }
    edges.push_back(CausalEdge{from, to, kind});
  };

  std::unordered_map<CorrelationId, std::uint32_t> last_by_correlation;
  last_by_correlation.reserve(graph.by_correlation_.size());
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    const auto node = static_cast<std::uint32_t>(i);
    const Record& record = graph.records[i];
    if (record.cause_seq != 0) {
      const auto cause = graph.by_seq_.find(record.cause_seq);
      if (cause != graph.by_seq_.end()) {
        add_edge(cause->second, node, CausalEdge::Kind::Cause);
      }
    }
    if (record.correlation != kNoCorrelation) {
      const auto previous = last_by_correlation.find(record.correlation);
      if (previous != last_by_correlation.end()) {
        add_edge(previous->second, node, CausalEdge::Kind::Correlation);
      }
      last_by_correlation[record.correlation] = node;
    }
    if (record.kind == RecordKind::SpanBegin && record.parent != kNoSpan) {
      const SpanKey parent_key{record.trace.high, record.trace.low,
                               record.parent};
      const auto found = spans.find(parent_key);
      if (found == spans.end()) {
        ++graph.unresolved_parents;
      } else {
        add_edge(found->second, node, CausalEdge::Kind::Parent);
      }
    }
    if (record.kind == RecordKind::SpanEnd) {
      const auto begin = spans.find(span_key(record));
      if (begin != spans.end()) {
        add_edge(begin->second, node, CausalEdge::Kind::Span);
      }
    }
  }

  std::sort(edges.begin(), edges.end(),
            [](const CausalEdge& a, const CausalEdge& b) {
              if (a.from != b.from) {
                return a.from < b.from;
              }
              if (a.to != b.to) {
                return a.to < b.to;
              }
              return static_cast<int>(a.kind) < static_cast<int>(b.kind);
            });
  edges.erase(std::unique(edges.begin(), edges.end(),
                          [](const CausalEdge& a, const CausalEdge& b) {
                            return a.from == b.from && a.to == b.to &&
                                   a.kind == b.kind;
                          }),
              edges.end());
  graph.edges = std::move(edges);

  graph.incoming.resize(graph.records.size());
  graph.outgoing.resize(graph.records.size());
  for (std::size_t i = 0; i < graph.edges.size(); ++i) {
    const CausalEdge& edge = graph.edges[i];
    graph.incoming[edge.to].push_back(static_cast<std::uint32_t>(i));
    graph.outgoing[edge.from].push_back(static_cast<std::uint32_t>(i));
  }
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    if (graph.incoming[i].empty()) {
      graph.roots.push_back(static_cast<std::uint32_t>(i));
    }
  }
  if (error != nullptr) {
    error->clear();
  }
  return graph;
}

GraphLayout layout_causal_graph(const CausalGraph& graph) {
  GraphLayout layout;
  const std::size_t count = graph.records.size();
  if (count == 0) {
    return layout;
  }
  std::vector<std::uint32_t> layers(count, 0);
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t best = 0;
    for (const std::uint32_t edge_index : graph.incoming[i]) {
      const CausalEdge& edge = graph.edges[edge_index];
      best = std::max(best, layers[edge.from] + 1);
    }
    layers[i] = best;
  }
  std::uint32_t max_layer = 0;
  for (std::uint32_t layer : layers) {
    max_layer = std::max(max_layer, layer);
  }
  std::vector<std::uint32_t> rows(static_cast<std::size_t>(max_layer) + 1, 0);
  layout.points.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t layer = layers[i];
    const std::uint32_t row = rows[layer]++;
    layout.points[i].layer = layer;
    layout.points[i].row = row;
    layout.points[i].x = static_cast<float>(layer) * 220.0f;
    layout.points[i].y = static_cast<float>(row) * 64.0f + 40.0f;
  }
  layout.layers = max_layer + 1;
  for (std::uint32_t row_count : rows) {
    layout.widest_layer = std::max(layout.widest_layer, row_count);
  }
  return layout;
}

std::vector<SpanRow> span_waterfall(const CausalGraph& graph,
                                    std::size_t limit) {
  std::vector<SpanRow> rows;
  if (limit == 0) {
    return rows;
  }
  std::unordered_map<SpanKey, std::uint32_t, SpanKeyHash> begins;
  std::unordered_map<SpanKey, std::uint32_t, SpanKeyHash> ends;
  begins.reserve(graph.records.size() / 2 + 1);
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    const Record& record = graph.records[i];
    if (record.kind == RecordKind::SpanBegin) {
      begins.emplace(span_key(record), static_cast<std::uint32_t>(i));
    } else if (record.kind == RecordKind::SpanEnd) {
      ends[span_key(record)] = static_cast<std::uint32_t>(i);
    }
  }
  rows.reserve(begins.size());
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    const Record& begin = graph.records[i];
    if (begin.kind != RecordKind::SpanBegin) {
      continue;
    }
    SpanRow row;
    row.trace = begin.trace;
    row.span = begin.span;
    row.parent = begin.parent;
    row.name = std::string(record_name(begin));
    row.start_mono_ns = begin.mono_ns;
    row.begin_node = static_cast<std::uint32_t>(i);
    const auto end = ends.find(span_key(begin));
    if (end != ends.end()) {
      const Record& end_record = graph.records[end->second];
      row.end_node = end->second;
      row.duration_ns = end_record.duration_ns;
      row.status = end_record.status;
    }
    SpanId parent = begin.parent;
    std::uint32_t depth = 0;
    while (parent != kNoSpan && depth < 64) {
      const SpanKey parent_key{begin.trace.high, begin.trace.low, parent};
      const auto found = begins.find(parent_key);
      if (found == begins.end()) {
        break;
      }
      ++depth;
      parent = graph.records[found->second].parent;
    }
    row.depth = depth;
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const SpanRow& a, const SpanRow& b) {
    if (a.start_mono_ns != b.start_mono_ns) {
      return a.start_mono_ns < b.start_mono_ns;
    }
    if (a.name != b.name) {
      return a.name < b.name;
    }
    return a.span < b.span;
  });
  if (rows.size() > limit) {
    rows.resize(limit);
  }
  return rows;
}

FailureExplanation explain_failure(const CausalGraph& graph,
                                   std::uint32_t error_node) {
  FailureExplanation explanation;
  if (error_node >= graph.records.size()) {
    explanation.summary = "error node is out of range";
    return explanation;
  }
  explanation.found = true;
  explanation.error_node = error_node;
  std::vector<std::uint32_t> reverse_chain;
  std::vector<bool> visited(graph.records.size(), false);
  std::uint32_t current = error_node;
  for (std::size_t step = 0; step < kMaxExplainSteps; ++step) {
    reverse_chain.push_back(current);
    visited[current] = true;
    std::uint32_t next = kInvalidNode;
    int best_priority = 5;
    for (const std::uint32_t edge_index : graph.incoming[current]) {
      const CausalEdge& edge = graph.edges[edge_index];
      if (visited[edge.from]) {
        continue;
      }
      const int priority = edge_priority(edge.kind);
      if (priority < best_priority) {
        best_priority = priority;
        next = edge.from;
      }
    }
    if (next == kInvalidNode) {
      break;
    }
    current = next;
  }
  explanation.chain.assign(reverse_chain.rbegin(), reverse_chain.rend());
  explanation.root_cause = explanation.chain.front();
  for (const std::uint32_t node : explanation.chain) {
    if (record_is_abnormal(graph.records[node])) {
      explanation.root_cause = node;
      break;
    }
  }
  explanation.findings.reserve(explanation.chain.size());
  for (const std::uint32_t node : explanation.chain) {
    explanation.findings.push_back(node_description(graph.records[node]));
  }
  const Record& root = graph.records[explanation.root_cause];
  explanation.summary = "root cause at seq " + std::to_string(root.seq) +
                        ": " + node_description(root) + "; " +
                        std::to_string(explanation.chain.size() - 1) +
                        " causal step(s) before the failure";
  return explanation;
}

FailureExplanation explain_latest_failure(const CausalGraph& graph) {
  FailureExplanation explanation;
  for (std::size_t i = graph.records.size(); i > 0; --i) {
    const Record& record = graph.records[i - 1];
    if (record_is_abnormal(record)) {
      return explain_failure(graph, static_cast<std::uint32_t>(i - 1));
    }
  }
  explanation.summary = "no error or timeout record in the trace";
  return explanation;
}

std::vector<StateHistory> state_timeline(const CausalGraph& graph,
                                        std::size_t max_keys,
                                        std::size_t max_transitions_per_key) {
  std::vector<StateHistory> histories;
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    const Record& record = graph.records[i];
    if (record.kind != RecordKind::StateChange) {
      continue;
    }
    const std::string key(record_state_key(record));
    auto found = index.find(key);
    if (found == index.end()) {
      if (histories.size() >= max_keys) {
        continue;
      }
      found = index.emplace(key, histories.size()).first;
      histories.push_back(StateHistory{key, {}});
    }
    StateHistory& history = histories[found->second];
    if (history.transitions.size() >= max_transitions_per_key) {
      continue;
    }
    StateTransition transition;
    transition.seq = record.seq;
    transition.wall_ns = record.wall_ns;
    transition.node = static_cast<std::uint32_t>(i);
    transition.state_key = key;
    transition.previous = owned_value(record.state_old);
    transition.next = owned_value(record.state_new);
    history.transitions.push_back(std::move(transition));
  }
  return histories;
}

TraceComparison compare_traces(const CausalGraph& left,
                               const CausalGraph& right) {
  TraceComparison comparison;
  std::size_t index = 0;
  while (index < left.records.size() && index < right.records.size()) {
    const Record& a = left.records[index];
    const Record& b = right.records[index];
    if (a.kind != b.kind || a.level != b.level || a.status != b.status ||
        record_name(a) != record_name(b) ||
        record_state_key(a) != record_state_key(b) ||
        !(owned_value(a.state_new) == owned_value(b.state_new)) ||
        record_message(a) != record_message(b)) {
      comparison.event_divergence = true;
      comparison.event_index = index;
      comparison.left_event = static_cast<std::uint32_t>(index);
      comparison.right_event = static_cast<std::uint32_t>(index);
      comparison.event_detail = shape_detail(a, b);
      break;
    }
    ++index;
  }
  if (!comparison.event_divergence &&
      left.records.size() != right.records.size()) {
    comparison.event_divergence = true;
    comparison.event_index = index;
    const bool left_shorter = left.records.size() < right.records.size();
    comparison.left_event =
        left_shorter ? kInvalidNode : static_cast<std::uint32_t>(index);
    comparison.right_event =
        left_shorter ? static_cast<std::uint32_t>(index) : kInvalidNode;
    comparison.event_detail =
        left_shorter ? "left trace ends earlier" : "right trace ends earlier";
  }

  const auto left_histories = state_timeline(left, 256, 4096);
  const auto right_histories = state_timeline(right, 256, 4096);
  const auto right_index = history_index(right_histories);
  for (const StateHistory& history : left_histories) {
    const auto found = right_index.find(history.key);
    if (found == right_index.end()) {
      comparison.state_divergence = true;
      comparison.state_key = history.key;
      comparison.state_index = 0;
      comparison.left_value =
          history.transitions.empty() ? Value() : history.transitions[0].next;
      comparison.right_value = Value();
      comparison.state_detail =
          "state key '" + history.key + "' is missing from the right trace";
      break;
    }
    const StateHistory& other = *found->second;
    std::size_t transition_index = 0;
    bool diverged = false;
    while (transition_index < history.transitions.size() &&
           transition_index < other.transitions.size()) {
      const StateTransition& a = history.transitions[transition_index];
      const StateTransition& b = other.transitions[transition_index];
      if (!(a.previous == b.previous) || !(a.next == b.next)) {
        comparison.state_divergence = true;
        comparison.state_key = history.key;
        comparison.state_index = transition_index;
        comparison.left_value = a.next;
        comparison.right_value = b.next;
        comparison.state_detail =
            "state '" + history.key + "' transition " +
            std::to_string(transition_index) + " differs ('" +
            a.next.display() + "' vs '" + b.next.display() + "')";
        diverged = true;
        break;
      }
      ++transition_index;
    }
    if (diverged) {
      break;
    }
    if (history.transitions.size() != other.transitions.size()) {
      comparison.state_divergence = true;
      comparison.state_key = history.key;
      comparison.state_index =
          std::min(history.transitions.size(), other.transitions.size());
      const bool left_shorter =
          history.transitions.size() < other.transitions.size();
      comparison.left_value = left_shorter ? Value()
                                           : history.transitions[transition_index].next;
      comparison.right_value = left_shorter
                                   ? other.transitions[transition_index].next
                                   : Value();
      comparison.state_detail =
          "state '" + history.key + "' has a different number of transitions";
      break;
    }
  }
  if (!comparison.state_divergence) {
    std::unordered_map<std::string, bool> known;
    for (const StateHistory& history : left_histories) {
      known[history.key] = true;
    }
    for (const StateHistory& history : right_histories) {
      if (known.find(history.key) == known.end()) {
        comparison.state_divergence = true;
        comparison.state_key = history.key;
        comparison.state_index = 0;
        comparison.left_value = Value();
        comparison.right_value =
            history.transitions.empty() ? Value() : history.transitions[0].next;
        comparison.state_detail =
            "state key '" + history.key + "' is missing from the left trace";
        break;
      }
    }
  }
  return comparison;
}

}  // namespace bb
