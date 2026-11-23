#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bb {

/// Bounded interning table used by the dump writer. Strings longer than
/// kMaxStringBytes and tables at capacity return kInvalidId instead of
/// truncating or growing without limit. Not thread-safe; the dump writer owns
/// one instance.
class StringTable {
 public:
  static constexpr std::uint32_t kInvalidId = 0xFFFFFFFFu;
  static constexpr std::size_t kMaxStrings = 16'384;
  static constexpr std::size_t kMaxStringBytes = 256;

  /// Interns `text`, returning its id. Ids start at zero. Returns kInvalidId if
  /// the text is too long or the table is full.
  std::uint32_t intern(std::string_view text);

  /// Returns the id of an already interned string, or kInvalidId.
  std::uint32_t lookup(std::string_view text) const;

  /// Resolves an id. Returns false for out-of-range ids.
  bool get(std::uint32_t id, std::string_view& out) const;

  std::size_t size() const noexcept { return strings_.size(); }
  void clear();
  std::size_t memory_bytes() const noexcept;

  const std::vector<std::string>& strings() const noexcept { return strings_; }

 private:
  std::vector<std::string> strings_;
  std::unordered_map<std::string, std::uint32_t> index_;
};

}  // namespace bb
