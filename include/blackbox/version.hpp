#pragma once

#include <string>
#include <string_view>

namespace bb {

/// Semantic version of the library, without a leading "v".
inline constexpr std::string_view kVersion = BLACKBOX_VERSION;

/// Returns a human readable version and platform description.
/// Thread-safe: the returned string is built on each call.
std::string build_info();

}  // namespace bb
