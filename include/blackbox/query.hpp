#pragma once

#include <blackbox/record.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bb {

/// Immutable parsed query. Copying is cheap (the syntax tree is shared) and
/// thread-safe. A default-constructed Query matches every record.
///
/// Grammar (case-insensitive keywords):
///   expr    := or
///   or      := and ("OR" and)*
///   and     := unary ("AND" unary)*
///   unary   := "NOT" unary | "(" expr ")" | predicate
///   predicate := field op value
///
/// Fields: kind, level, status, name, message, text/any, trace, span, parent,
/// correlation/corr, seq, cause, duration, state/state_key, state.old,
/// state.new, value, attr.KEY or attr["KEY"].
///
/// Operators: = (or ==), !=, <, <=, >, >=, ~ (case-insensitive contains),
/// !~ (does not contain). Numeric fields accept integers; duration accepts an
/// integer in nanoseconds or a unit suffix (ns, us, ms, s). Kind, level and
/// status compare against their names. Names and messages are case-sensitive
/// for = and !=; ~ is ASCII case-insensitive.
///
/// Missing attributes never match, including with !=. Hard limits: 4096 source
/// bytes, 1024 tokens, 512 syntax nodes, 32 nesting levels and 512-byte string
/// literals. Parse failures are explicit; nothing is truncated.
class Query {
 public:
  struct Node;

  Query() = default;
  Query(const Query&) = default;
  Query(Query&&) noexcept = default;
  Query& operator=(const Query&) = default;
  Query& operator=(Query&&) noexcept = default;
  ~Query() = default;

  /// Parses `source`. An empty or whitespace-only query matches everything.
  /// Returns nullopt and fills `error` on malformed input.
  static std::optional<Query> parse(std::string_view source,
                                    std::string* error = nullptr);

  bool matches(const Record& record) const;
  bool match_all() const noexcept { return root_ == nullptr; }
  const std::string& source() const noexcept { return source_; }

 private:
  std::shared_ptr<const Node> root_;
  std::string source_;
};

/// Returns copies of records matching `query`, up to max_results.
std::vector<Record> filter_records(std::span<const Record> records,
                                   const Query& query,
                                   std::size_t max_results = 100'000);

}  // namespace bb
