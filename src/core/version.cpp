#include <blackbox/version.hpp>

namespace bb {

std::string build_info() {
  std::string out = "blackbox ";
  out += kVersion;
  out += " (";
#if defined(BLACKBOX_PLATFORM_MACOS)
  out += "macOS";
#elif defined(BLACKBOX_PLATFORM_POSIX)
  out += "POSIX";
#else
  out += "unknown platform";
#endif
  out += ", C++20)";
  return out;
}

}  // namespace bb
