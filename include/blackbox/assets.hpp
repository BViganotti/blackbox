#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace bb::assets {

struct Asset {
  std::string_view path;
  std::string_view content_type;
  std::span<const unsigned char> bytes;
};

/// Every compiled-in UI asset, in a deterministic order.
std::span<const Asset> all();

/// Looks up an asset by its exact path (for example "/app.js" or "app.js").
/// Returns nullptr for unknown paths.
const Asset* find(std::string_view path);

}  // namespace bb::assets
