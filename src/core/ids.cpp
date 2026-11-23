#include <blackbox/ids.hpp>
#include <blackbox/util/random.hpp>

#include <atomic>
#include <cstdlib>

namespace bb {

TraceId TraceId::random() noexcept {
  TraceId id;
  do {
    if (!fill_random(&id, sizeof(id))) {
      std::abort();
    }
  } while (id.is_zero());
  return id;
}

namespace {

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

std::optional<TraceId> TraceId::from_hex(std::string_view text) noexcept {
  if (text.size() != 32) {
    return std::nullopt;
  }
  std::uint64_t words[2] = {0, 0};
  for (std::size_t i = 0; i < 32; ++i) {
    const int digit = hex_digit(text[i]);
    if (digit < 0) {
      return std::nullopt;
    }
    words[i / 16] = (words[i / 16] << 4) | static_cast<std::uint64_t>(digit);
  }
  TraceId id{words[0], words[1]};
  if (id.is_zero()) {
    return std::nullopt;
  }
  return id;
}

std::string TraceId::to_hex() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(32, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    const std::uint64_t word = i < 16 ? high : low;
    const int shift = static_cast<int>(15 - (i % 16)) * 4;
    out[i] = kHex[(word >> shift) & 0xF];
  }
  return out;
}

CorrelationId new_correlation_id() noexcept {
  CorrelationId id = 0;
  do {
    if (!fill_random(&id, sizeof(id))) {
      std::abort();
    }
  } while (id == 0);
  return id;
}

SpanId next_span_id() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  std::uint64_t id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  if (id == 0) {
    id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  return id;
}

}  // namespace bb
