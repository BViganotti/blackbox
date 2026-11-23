#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bb {

/// Lowercase hex encoding. `out` receives exactly 2 * data.size() characters and
/// is returned for convenience.
std::string to_hex(std::span<const std::byte> data);
std::string to_hex(const void* data, std::size_t size);

/// Compares two strings without early exit on the first differing byte.
/// Intended for authentication tokens. Length differences still leak length.
bool secure_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace bb
