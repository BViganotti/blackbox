#include <blackbox/version.hpp>

#include <cstdio>

int main() {
  std::printf("blackbox fuzz harness (%s)\n", bb::build_info().c_str());
  std::printf("no fuzz targets registered yet\n");
  return 0;
}
