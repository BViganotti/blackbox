#include <blackbox/util/random.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif

#include <fcntl.h>
#include <unistd.h>

namespace bb {
namespace {

bool fill_from_getentropy(void* data, std::size_t size) noexcept {
#if defined(__APPLE__) || defined(__linux__)
  auto* bytes = static_cast<unsigned char*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t chunk = size - offset > 256 ? 256 : size - offset;
    if (getentropy(bytes + offset, chunk) != 0) {
      return false;
    }
    offset += chunk;
  }
  return true;
#else
  (void)data;
  (void)size;
  return false;
#endif
}

bool fill_from_urandom(void* data, std::size_t size) noexcept {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  auto* bytes = static_cast<unsigned char*>(data);
  std::size_t offset = 0;
  bool ok = true;
  while (offset < size) {
    const ssize_t got = ::read(fd, bytes + offset, size - offset);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    if (got == 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(got);
  }
  ::close(fd);
  return ok;
}

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

bool fill_random(void* data, std::size_t size) noexcept {
  if (size == 0) {
    return true;
  }
  if (fill_from_getentropy(data, size)) {
    return true;
  }
  return fill_from_urandom(data, size);
}

std::uint64_t random_u64() noexcept {
  std::uint64_t value = 0;
  if (!fill_random(&value, sizeof(value))) {
    std::abort();
  }
  return value;
}

std::string random_token(std::size_t bytes) {
  if (bytes < 1) {
    bytes = 1;
  }
  if (bytes > 64) {
    bytes = 64;
  }
  unsigned char raw[64];
  if (!fill_random(raw, bytes)) {
    throw std::runtime_error("operating system random source unavailable");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.resize(bytes * 2);
  for (std::size_t i = 0; i < bytes; ++i) {
    token[i * 2] = kHex[raw[i] >> 4];
    token[i * 2 + 1] = kHex[raw[i] & 0x0F];
  }
  return token;
}

Xoshiro128::Xoshiro128(std::uint64_t seed) noexcept {
  std::uint64_t state = seed;
  for (std::uint32_t& word : state_) {
    const std::uint64_t value = splitmix64(state);
    word = static_cast<std::uint32_t>(value >> 32);
  }
}

std::uint32_t Xoshiro128::next_u32() noexcept {
  const std::uint32_t result = ((state_[1] * 5u) << 7 | (state_[1] * 5u) >> 25);
  const std::uint32_t t = state_[1] << 9;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = (state_[3] << 11) | (state_[3] >> 21);
  return result;
}

std::uint64_t Xoshiro128::next_u64() noexcept {
  const std::uint64_t high = next_u32();
  const std::uint64_t low = next_u32();
  return (high << 32) | low;
}

std::uint32_t Xoshiro128::bounded(std::uint32_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  // Rejection sampling avoids modulo bias.
  const std::uint32_t threshold = static_cast<std::uint32_t>(-bound) % bound;
  for (;;) {
    const std::uint32_t value = next_u32();
    if (value >= threshold) {
      return value % bound;
    }
  }
}

}  // namespace bb
