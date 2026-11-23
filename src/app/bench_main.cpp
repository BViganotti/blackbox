#include <blackbox/version.hpp>

#include <cstdio>

int main() {
  std::printf("%s\n", bb::build_info().c_str());
  return 0;
}
