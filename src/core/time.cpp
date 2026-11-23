#include <blackbox/util/time.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace bb {
namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kSecondsPerDay = 86'400;

constexpr std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
  std::int64_t quotient = value / divisor;
  if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
    --quotient;
  }
  return quotient;
}

constexpr std::int64_t floor_mod(std::int64_t value, std::int64_t divisor) {
  return value - floor_div(value, divisor) * divisor;
}

struct CivilDate {
  int year;
  unsigned month;
  unsigned day;
};

// Howard Hinnant's civil calendar algorithms, implemented directly from the
// proleptic Gregorian rules. Valid for the full int64 range of days.
constexpr CivilDate civil_from_days(std::int64_t days) {
  std::int64_t z = days + 719468;
  const std::int64_t era = floor_div(z, 146097);
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  year += month <= 2 ? 1 : 0;
  return CivilDate{year, month, day};
}

constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = floor_div(year, 400);
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
  const unsigned doy = (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr bool is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr unsigned days_in_month(int year, unsigned month) {
  constexpr unsigned kDays[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kDays[month - 1];
}

bool read_digits(std::string_view text, std::size_t offset, std::size_t count,
                 int& value) {
  value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  return true;
}

std::string scale_duration(std::uint64_t magnitude, std::uint64_t unit,
                           const char* suffix) {
  const std::uint64_t whole = magnitude / unit;
  const std::uint64_t fraction = (magnitude % unit) * 1000 / unit;
  char buffer[64];
  if (fraction == 0) {
    std::snprintf(buffer, sizeof(buffer), "%llu%s",
                  static_cast<unsigned long long>(whole), suffix);
  } else {
    std::uint64_t trimmed = fraction;
    int digits = 3;
    while (digits > 1 && trimmed % 10 == 0) {
      trimmed /= 10;
      --digits;
    }
    std::snprintf(buffer, sizeof(buffer), "%llu.%0*llu%s",
                  static_cast<unsigned long long>(whole), digits,
                  static_cast<unsigned long long>(trimmed), suffix);
  }
  return buffer;
}

}  // namespace

int64_t now_wall_ns() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

int64_t now_mono_ns() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::string format_wall_ns(int64_t ns) {
  const std::int64_t seconds = floor_div(ns, kNsPerSecond);
  const std::int64_t nanos = floor_mod(ns, kNsPerSecond);
  const std::int64_t days = floor_div(seconds, kSecondsPerDay);
  const std::int64_t second_of_day = floor_mod(seconds, kSecondsPerDay);
  const CivilDate date = civil_from_days(days);
  const int hour = static_cast<int>(second_of_day / 3600);
  const int minute = static_cast<int>((second_of_day % 3600) / 60);
  const int second = static_cast<int>(second_of_day % 60);
  const int millis = static_cast<int>(nanos / 1'000'000);
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ",
                date.year, date.month, date.day, hour, minute, second, millis);
  return buffer;
}

std::optional<int64_t> parse_wall_ns(std::string_view text) {
  // Accepted forms: YYYY-MM-DDTHH:MM:SSZ (20) or with .mmm (24).
  if (text.size() != 20 && text.size() != 24) {
    return std::nullopt;
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text.back() != 'Z') {
    return std::nullopt;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!read_digits(text, 0, 4, year) || !read_digits(text, 5, 2, month) ||
      !read_digits(text, 8, 2, day) || !read_digits(text, 11, 2, hour) ||
      !read_digits(text, 14, 2, minute) ||
      !read_digits(text, 17, 2, second)) {
    return std::nullopt;
  }
  if (text.size() == 24 && text[19] != '.') {
    return std::nullopt;
  }
  int millis = 0;
  if (text.size() == 24 && !read_digits(text, 20, 3, millis)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59 ||
      day < 1 || static_cast<unsigned>(day) > days_in_month(year, static_cast<unsigned>(month))) {
    return std::nullopt;
  }
  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
  const std::int64_t seconds = days * kSecondsPerDay + hour * 3600 + minute * 60 + second;
  return seconds * kNsPerSecond + static_cast<std::int64_t>(millis) * 1'000'000;
}

std::string format_duration_ns(int64_t ns) {
  const bool negative = ns < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(ns + 1)) + 1
               : static_cast<std::uint64_t>(ns);
  std::string result;
  if (magnitude < 1'000) {
    result = std::to_string(magnitude) + "ns";
  } else if (magnitude < 1'000'000) {
    result = scale_duration(magnitude, 1'000, "us");
  } else if (magnitude < 1'000'000'000) {
    result = scale_duration(magnitude, 1'000'000, "ms");
  } else {
    result = scale_duration(magnitude, 1'000'000'000, "s");
  }
  if (negative) {
    result.insert(result.begin(), '-');
  }
  return result;
}

}  // namespace bb
