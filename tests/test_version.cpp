#include "harness.hpp"

#include <blackbox/version.hpp>

#include <string>

BB_TEST(version_is_not_empty) {
  CHECK(!bb::kVersion.empty());
  CHECK(bb::kVersion.find('.') != std::string_view::npos);
}

BB_TEST(build_info_mentions_product) {
  const std::string info = bb::build_info();
  CHECK(info.find("blackbox") == 0);
  CHECK(info.find(bb::kVersion) != std::string::npos);
}
