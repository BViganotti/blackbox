#include "harness.hpp"

#include <exception>
#include <iostream>

namespace bbtest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void fail(const char* file, int line, const std::string& expression,
          const std::string& detail) {
  std::ostringstream message;
  message << file << ':' << line << ": check failed: " << expression;
  if (!detail.empty()) {
    message << " (" << detail << ')';
  }
  throw Failure(message.str());
}

}  // namespace bbtest

int main() {
  int failed = 0;
  int ran = 0;
  for (const auto& test : bbtest::registry()) {
    ++ran;
    try {
      test.fn();
      std::cout << "PASS " << test.name << '\n';
    } catch (const bbtest::Failure& failure) {
      ++failed;
      std::cout << "FAIL " << test.name << "\n  " << failure.what() << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cout << "FAIL " << test.name << "\n  unexpected exception: "
                << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cout << "FAIL " << test.name << "\n  unknown exception\n";
    }
  }
  std::cout << (failed == 0 ? "OK " : "FAILED ") << ran - failed << '/' << ran
            << " tests passed\n";
  return failed == 0 ? 0 : 1;
}
