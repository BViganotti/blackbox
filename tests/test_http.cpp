#include "harness.hpp"

#include "ui/http.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

int make_pair(int fds[2]) {
  return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

bool send_text(int fd, const std::string& text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t sent = ::send(fd, text.data() + offset, text.size() - offset, 0);
    if (sent <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

std::string receive_all(int fd) {
  std::string data;
  std::array<char, 2048> buffer{};
  for (;;) {
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got <= 0) {
      break;
    }
    data.append(buffer.data(), static_cast<std::size_t>(got));
    if (data.size() > 1024u * 1024u) {
      break;
    }
  }
  return data;
}

}  // namespace

BB_TEST(http_url_decode_is_bounded_and_strict) {
  std::string out;
  CHECK(bb::ui::url_decode("a%20b+c", true, out));
  CHECK_EQ(out, std::string("a b c"));
  CHECK(bb::ui::url_decode("a%20b+c", false, out));
  CHECK_EQ(out, std::string("a b+c"));
  CHECK(bb::ui::url_decode("", true, out));
  CHECK(out.empty());
  CHECK(!bb::ui::url_decode("%2", true, out));
  CHECK(!bb::ui::url_decode("%zz", true, out));
  CHECK(!bb::ui::url_decode("%00", true, out));
  CHECK(!bb::ui::url_decode(std::string_view("a\0b", 3), true, out));
  CHECK(bb::ui::url_decode("%E2%82%AC", true, out));
  CHECK_EQ(out, std::string("\xE2\x82\xAC"));
}

BB_TEST(http_parse_u64_rejects_overflow) {
  std::uint64_t value = 1;
  CHECK(bb::ui::parse_u64("0", value));
  CHECK_EQ(value, 0u);
  CHECK(bb::ui::parse_u64("18446744073709551615", value));
  CHECK_EQ(value, UINT64_MAX);
  CHECK(!bb::ui::parse_u64("18446744073709551616", value));
  CHECK(!bb::ui::parse_u64("", value));
  CHECK(!bb::ui::parse_u64("-1", value));
  CHECK(!bb::ui::parse_u64("12x", value));
  CHECK(!bb::ui::parse_u64("0000000000000000000001", value));
}

BB_TEST(http_reads_a_valid_request_with_params) {
  int fds[2] = {-1, -1};
  CHECK(make_pair(fds) == 0);
  const std::string request =
      "GET /api/snapshot?q=kind%3Derror&limit=5&token=abc HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "X-Blackbox-Token: secret\r\n"
      "Content-Length: 0\r\n\r\n";
  CHECK(send_text(fds[0], request));
  bb::ui::Request parsed;
  std::string error;
  CHECK(bb::ui::read_request(fds[1], parsed, error));
  CHECK_EQ(parsed.method, std::string("GET"));
  CHECK_EQ(parsed.path, std::string("/api/snapshot"));
  CHECK_EQ(parsed.version, std::string("HTTP/1.1"));
  CHECK_EQ(*parsed.param("q"), std::string("kind=error"));
  CHECK_EQ(*parsed.param("limit"), std::string("5"));
  CHECK_EQ(*parsed.header("x-blackbox-token"), std::string("secret"));
  CHECK(parsed.header("missing") == nullptr);
  ::close(fds[0]);
  ::close(fds[1]);
}

BB_TEST(http_rejects_malformed_requests) {
  const std::vector<std::string> invalid = {
      "GARBAGE\r\n\r\n",
      "PUT / HTTP/1.1\r\n\r\n",
      "GET / HTTP/2.0\r\n\r\n",
      "GET relative HTTP/1.1\r\n\r\n",
      "GET / HTTP/1.1\r\n folded: value\r\n\r\n",
      "GET / HTTP/1.1\r\nNoColon\r\n\r\n",
      "GET /api?bad=%zz HTTP/1.1\r\n\r\n",
      "POST / HTTP/1.1\r\nContent-Length: 99999\r\n\r\n",
      "POST / HTTP/1.1\r\nContent-Length: nope\r\n\r\n",
  };
  for (const auto& text : invalid) {
    int fds[2] = {-1, -1};
    CHECK(make_pair(fds) == 0);
    CHECK(send_text(fds[0], text));
    ::shutdown(fds[0], SHUT_WR);
    bb::ui::Request parsed;
    std::string error;
    CHECK(!bb::ui::read_request(fds[1], parsed, error));
    CHECK(!error.empty());
    ::close(fds[0]);
    ::close(fds[1]);
  }
}

BB_TEST(http_rejects_too_many_parameters_and_headers) {
  std::string params;
  for (int i = 0; i < 40; ++i) {
    params += "p" + std::to_string(i) + "=v&";
  }
  int fds[2] = {-1, -1};
  CHECK(make_pair(fds) == 0);
  CHECK(send_text(fds[0], "GET /?" + params + " HTTP/1.1\r\n\r\n"));
  ::shutdown(fds[0], SHUT_WR);
  bb::ui::Request parsed;
  std::string error;
  CHECK(!bb::ui::read_request(fds[1], parsed, error));
  ::close(fds[0]);
  ::close(fds[1]);

  std::string headers;
  for (int i = 0; i < 70; ++i) {
    headers += "X-H" + std::to_string(i) + ": v\r\n";
  }
  CHECK(make_pair(fds) == 0);
  CHECK(send_text(fds[0], "GET / HTTP/1.1\r\n" + headers + "\r\n"));
  ::shutdown(fds[0], SHUT_WR);
  error.clear();
  CHECK(!bb::ui::read_request(fds[1], parsed, error));
  ::close(fds[0]);
  ::close(fds[1]);
}

BB_TEST(http_response_serialisation_and_head) {
  int fds[2] = {-1, -1};
  CHECK(make_pair(fds) == 0);
  bb::ui::Response response;
  response.status = 201;
  response.content_type = "text/plain";
  response.body = "hello";
  response.extra_headers.emplace_back("Set-Cookie", "a=b");
  CHECK(bb::ui::send_response(fds[0], response));
  ::shutdown(fds[0], SHUT_WR);
  const std::string wire = receive_all(fds[1]);
  CHECK(wire.find("HTTP/1.1 201 Created\r\n") == 0);
  CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
  CHECK(wire.find("Set-Cookie: a=b\r\n") != std::string::npos);
  CHECK(wire.find("\r\n\r\nhello") != std::string::npos);
  ::close(fds[0]);
  ::close(fds[1]);

  CHECK(make_pair(fds) == 0);
  CHECK(bb::ui::send_response(fds[0], response, true));
  ::shutdown(fds[0], SHUT_WR);
  const std::string head = receive_all(fds[1]);
  CHECK(head.find("Content-Length: 5\r\n") != std::string::npos);
  CHECK(head.find("\r\n\r\nhello") == std::string::npos);
  ::close(fds[0]);
  ::close(fds[1]);
}
