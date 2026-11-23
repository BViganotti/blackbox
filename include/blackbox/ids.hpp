#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bb {

/// 128-bit trace identifier. All-zero is reserved for "unset".
struct TraceId {
  std::uint64_t high = 0;
  std::uint64_t low = 0;

  /// Generates a random non-zero trace id from the operating system source.
  static TraceId random() noexcept;

  /// Parses exactly 32 lowercase or uppercase hex digits. Zero is rejected.
  static std::optional<TraceId> from_hex(std::string_view text) noexcept;

  [[nodiscard]] bool is_zero() const noexcept {
    return high == 0 && low == 0;
  }

  /// 32 lowercase hexadecimal digits.
  [[nodiscard]] std::string to_hex() const;
};

inline bool operator==(const TraceId& a, const TraceId& b) noexcept {
  return a.high == b.high && a.low == b.low;
}
inline bool operator!=(const TraceId& a, const TraceId& b) noexcept {
  return !(a == b);
}
inline bool operator<(const TraceId& a, const TraceId& b) noexcept {
  return a.high != b.high ? a.high < b.high : a.low < b.low;
}

/// Span identifiers are scoped to a trace. Zero means "no span".
using SpanId = std::uint64_t;
inline constexpr SpanId kNoSpan = 0;

/// Correlation identifiers tie together work that is not a strict parent/child
/// span relation, for example a logical request that fans out across threads.
using CorrelationId = std::uint64_t;
inline constexpr CorrelationId kNoCorrelation = 0;

/// Returns a random non-zero correlation id.
CorrelationId new_correlation_id() noexcept;

/// Returns a monotonically increasing span id unique within the process.
/// Thread-safe. Wraps only after 2^64 calls.
SpanId next_span_id() noexcept;

}  // namespace bb
