#pragma once

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bbtest {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, void (*fn)()) {
    registry().push_back(TestCase{name, fn});
  }
};

[[noreturn]] void fail(const char* file, int line, const std::string& expression,
                       const std::string& detail);

template <typename T>
std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string describe(const std::string& value) { return value; }
inline std::string describe(const char* value) { return value ? value : "<null>"; }

}  // namespace bbtest

#define BB_CONCAT_IMPL(a, b) a##b
#define BB_CONCAT(a, b) BB_CONCAT_IMPL(a, b)

#define BB_TEST(name)                                                     \
  static void BB_CONCAT(bbtest_fn_, name)();                              \
  static ::bbtest::Registrar BB_CONCAT(bbtest_reg_, name)(                \
      #name, &BB_CONCAT(bbtest_fn_, name));                               \
  static void BB_CONCAT(bbtest_fn_, name)()

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      ::bbtest::fail(__FILE__, __LINE__, #condition, "");                 \
    }                                                                     \
  } while (0)

#define CHECK_EQ(actual, expected)                                        \
  do {                                                                    \
    const auto bb_actual = (actual);                                      \
    const auto bb_expected = (expected);                                 \
    if (!(bb_actual == bb_expected)) {                                    \
      ::bbtest::fail(__FILE__, __LINE__, #actual " == " #expected,        \
                     ::bbtest::describe(bb_actual) + " != " +             \
                         ::bbtest::describe(bb_expected));                \
    }                                                                     \
  } while (0)

#define CHECK_NE(actual, expected)                                        \
  do {                                                                    \
    const auto bb_actual = (actual);                                      \
    const auto bb_expected = (expected);                                 \
    if (bb_actual == bb_expected) {                                       \
      ::bbtest::fail(__FILE__, __LINE__, #actual " != " #expected,        \
                     "both were " + ::bbtest::describe(bb_actual));       \
    }                                                                     \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                           \
  do {                                                                    \
    const double bb_a = static_cast<double>(actual);                      \
    const double bb_e = static_cast<double>(expected);                    \
    const double bb_t = static_cast<double>(tolerance);                   \
    const double bb_d = bb_a > bb_e ? bb_a - bb_e : bb_e - bb_a;          \
    if (!(bb_d <= bb_t)) {                                                \
      ::bbtest::fail(__FILE__, __LINE__, #actual " near " #expected,      \
                     ::bbtest::describe(bb_a) + " vs " +                  \
                         ::bbtest::describe(bb_e));                       \
    }                                                                     \
  } while (0)
