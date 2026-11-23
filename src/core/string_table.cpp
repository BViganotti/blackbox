#include <blackbox/string_table.hpp>

namespace bb {

std::uint32_t StringTable::intern(std::string_view text) {
  if (text.size() > kMaxStringBytes) {
    return kInvalidId;
  }
  const auto found = index_.find(std::string(text));
  if (found != index_.end()) {
    return found->second;
  }
  if (strings_.size() >= kMaxStrings) {
    return kInvalidId;
  }
  const auto id = static_cast<std::uint32_t>(strings_.size());
  strings_.emplace_back(text);
  index_.emplace(std::string(text), id);
  return id;
}

std::uint32_t StringTable::lookup(std::string_view text) const {
  if (text.size() > kMaxStringBytes) {
    return kInvalidId;
  }
  const auto found = index_.find(std::string(text));
  return found == index_.end() ? kInvalidId : found->second;
}

bool StringTable::get(std::uint32_t id, std::string_view& out) const {
  if (id >= strings_.size()) {
    return false;
  }
  out = strings_[id];
  return true;
}

void StringTable::clear() {
  strings_.clear();
  index_.clear();
}

std::size_t StringTable::memory_bytes() const noexcept {
  std::size_t total = sizeof(StringTable);
  total += strings_.capacity() * sizeof(std::string);
  for (const std::string& text : strings_) {
    total += text.capacity();
  }
  total += index_.size() * (sizeof(std::string) + sizeof(std::uint32_t) + 16);
  return total;
}

}  // namespace bb
