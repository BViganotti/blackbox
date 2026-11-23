#pragma once

#include <blackbox/attribute.hpp>
#include <blackbox/ids.hpp>
#include <blackbox/record.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bb {

/// Sentinel for "no node" in analysis results.
inline constexpr std::uint32_t kInvalidNode = 0xFFFFFFFFu;

/// Bounded causal graph built from a record sequence. All edges point to a
/// record with a larger sequence number, so the graph is a DAG by construction.
/// The graph owns a copy of the records, keeping analysis independent of the
/// recorder's ring.
struct CausalEdge {
  std::uint32_t from = kInvalidNode;
  std::uint32_t to = kInvalidNode;
  enum class Kind : std::uint8_t {
    Cause = 0,       ///< cause_seq link
    Parent = 1,      ///< parent span link
    Span = 2,        ///< span begin -> span end
    Correlation = 3, ///< consecutive records sharing a correlation id
  } kind = Kind::Cause;
};

const char* to_string(CausalEdge::Kind kind) noexcept;

struct CausalGraph {
  std::vector<Record> records;      ///< sorted by seq, unique
  std::vector<CausalEdge> edges;    ///< sorted and deduplicated
  std::vector<std::vector<std::uint32_t>> incoming;  ///< edge indices by node
  std::vector<std::vector<std::uint32_t>> outgoing;  ///< edge indices by node
  std::vector<std::uint32_t> roots; ///< nodes without incoming edges
  std::uint64_t unresolved_parents = 0;
  std::uint32_t max_depth = 0;

  std::size_t size() const noexcept { return records.size(); }
  bool empty() const noexcept { return records.empty(); }
  const Record& record(std::uint32_t node) const;
  std::optional<std::uint32_t> find_seq(std::uint64_t seq) const;
  std::vector<std::uint32_t> nodes_for_correlation(CorrelationId id) const;

 private:
  friend CausalGraph build_causal_graph(std::vector<Record>, std::string*);
  std::unordered_map<std::uint64_t, std::uint32_t> by_seq_;
  std::unordered_map<CorrelationId, std::vector<std::uint32_t>> by_correlation_;
  std::unordered_map<std::string, std::vector<std::uint32_t>> by_state_key_;
};

/// Builds the graph. Invalid, duplicate or too many records cause a clean
/// failure with an error message; analysis never runs on malformed data.
CausalGraph build_causal_graph(std::vector<Record> records,
                               std::string* error = nullptr);

struct GraphPoint {
  float x = 0.0f;
  float y = 0.0f;
  std::uint32_t layer = 0;
  std::uint32_t row = 0;
};

struct GraphLayout {
  std::vector<GraphPoint> points;
  std::uint32_t layers = 0;
  std::uint32_t widest_layer = 0;
};

/// Deterministic layered layout. Layer is the longest path from a root; rows
/// are ordered by sequence within a layer.
GraphLayout layout_causal_graph(const CausalGraph& graph);

struct SpanRow {
  TraceId trace;
  SpanId span = kNoSpan;
  SpanId parent = kNoSpan;
  std::string name;
  Status status = Status::Unset;
  std::int64_t start_mono_ns = 0;
  std::int64_t duration_ns = 0;
  std::uint32_t begin_node = kInvalidNode;
  std::uint32_t end_node = kInvalidNode;
  std::uint32_t depth = 0;
};

/// One row per span begin, sorted by start time then name. Unfinished spans have
/// duration 0 and an invalid end node.
std::vector<SpanRow> span_waterfall(const CausalGraph& graph,
                                    std::size_t limit = 4096);

struct FailureExplanation {
  bool found = false;
  std::uint32_t error_node = kInvalidNode;
  std::uint32_t root_cause = kInvalidNode;
  std::vector<std::uint32_t> chain;  ///< chronological, root cause first
  std::vector<std::string> findings;
  std::string summary;
};

/// Explains a failure by walking causal, span and parent edges backwards. The
/// root cause is the earliest timeout, error, failed span or abnormal state
/// transition in the chain, falling back to the earliest linked record.
FailureExplanation explain_failure(const CausalGraph& graph,
                                   std::uint32_t error_node);

/// Picks the latest error/timeout record (highest sequence) and explains it.
FailureExplanation explain_latest_failure(const CausalGraph& graph);

struct StateTransition {
  std::uint64_t seq = 0;
  std::int64_t wall_ns = 0;
  std::uint32_t node = kInvalidNode;
  std::string state_key;
  Value previous;
  Value next;
};

struct StateHistory {
  std::string key;
  std::vector<StateTransition> transitions;
};

/// Groups state changes by key in first-seen order. Bounded by max_keys and
/// max_transitions_per_key.
std::vector<StateHistory> state_timeline(const CausalGraph& graph,
                                        std::size_t max_keys = 64,
                                        std::size_t max_transitions_per_key = 256);

struct TraceComparison {
  bool event_divergence = false;
  std::size_t event_index = 0;
  std::uint32_t left_event = kInvalidNode;
  std::uint32_t right_event = kInvalidNode;
  std::string event_detail;

  bool state_divergence = false;
  std::string state_key;
  std::size_t state_index = 0;
  Value left_value;
  Value right_value;
  std::string state_detail;
};

/// Compares two traces by record shape (kind, name, level, status, state key and
/// new state value) and then by state transitions. Timestamps and ids are
/// ignored. The first difference wins.
TraceComparison compare_traces(const CausalGraph& left, const CausalGraph& right);

}  // namespace bb
