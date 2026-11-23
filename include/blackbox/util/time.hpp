#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bb {

/// Wall clock time from the host clock, as nanoseconds since the Unix epoch.
/// Not monotonic; use now_mono_ns() for durations.
int64_t now_wall_ns() noexcept;

/// Monotonic time as nanoseconds from an unspecified epoch. Suitable only for
/// measuring durations within one process.
int64_t now_mono_ns() noexcept;

/// Formats Unix nanoseconds as UTC ISO-8601 with millisecond precision,
/// for example "2026-11-23T12:34:56.789Z".
std::string format_wall_ns(int64_t ns);

/// Parses the exact UTC forms "YYYY-MM-DDTHH:MM:SSZ" and
/// "YYYY-MM-DDTHH:MM:SS.mmmZ". Returns nullopt for anything else, including
/// out-of-range fields and dates that do not exist.
std::optional<int64_t> parse_wall_ns(std::string_view text);

/// Formats a duration for humans using ns/us/ms/s units. Deterministic.
std::string format_duration_ns(int64_t ns);

}  // namespace bb
