#include "harness.hpp"
#include "support.hpp"

#include <blackbox/recorder.hpp>
#include <blackbox/ui_server.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

struct ClientResponse {
  bool ok = false;
  int status = 0;
  std::string headers;
  std::string body;
};

int connect_local(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct timeval timeout {};
  timeout.tv_sec = 5;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool send_string(int fd, const std::string& text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t sent = ::send(fd, text.data() + offset, text.size() - offset, 0);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

ClientResponse read_http_response(int fd) {
  std::string data;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (got == 0) {
      break;
    }
    data.append(buffer.data(), static_cast<std::size_t>(got));
    if (data.size() > 16u * 1024u * 1024u) {
      break;
    }
  }
  ClientResponse response;
  const std::size_t end = data.find("\r\n\r\n");
  if (end == std::string::npos) {
    return response;
  }
  response.headers = data.substr(0, end);
  response.body = data.substr(end + 4);
  const std::size_t first_space = response.headers.find(' ');
  if (first_space == std::string::npos ||
      response.headers.size() < first_space + 4) {
    return response;
  }
  const std::string code = response.headers.substr(first_space + 1, 3);
  response.status = std::atoi(code.c_str());
  response.ok = response.status > 0;
  return response;
}

ClientResponse request(std::uint16_t port, const std::string& raw) {
  ClientResponse failed;
  const int fd = connect_local(port);
  if (fd < 0) {
    return failed;
  }
  if (!send_string(fd, raw)) {
    ::close(fd);
    return failed;
  }
  ClientResponse response = read_http_response(fd);
  ::close(fd);
  return response;
}

ClientResponse get(std::uint16_t port, const std::string& target,
                   const std::string& extra_headers = {}) {
  std::string raw = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  raw += extra_headers;
  raw += "\r\n";
  return request(port, raw);
}

}  // namespace

BB_TEST(server_auth_assets_and_snapshot) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 64;
  config.dump_directory = temp.file("dumps");
  bb::Recorder recorder(config);
  CHECK(recorder.log(bb::Level::Info, "hello from test"));
  const auto span = recorder.begin_span("request");
  CHECK(span.valid());
  CHECK(recorder.error("boom", bb::Status::Timeout));
  CHECK(recorder.end_span(span, bb::Status::Error));
  CHECK(recorder.state_change("breaker.state", bb::Value::string("closed"),
                              bb::Value::string("open")));

  bb::UiServer server(recorder);
  std::string error;
  CHECK(server.start(&error));
  CHECK(error.empty());
  CHECK(server.running());
  CHECK_NE(server.port(), 0);
  const std::uint16_t port = server.port();
  const std::string& token = server.token();
  CHECK_EQ(token.size(), 32u);
  CHECK(server.url().find(token) != std::string::npos);

  const auto health = get(port, "/api/health");
  CHECK(health.ok);
  CHECK_EQ(health.status, 200);
  CHECK(health.body.find("\"product\":\"blackbox\"") != std::string::npos);
  CHECK(health.body.find("\"uptime_ms\":") != std::string::npos);

  const auto unauthorized_stats = get(port, "/api/stats");
  CHECK_EQ(unauthorized_stats.status, 401);
  const auto stats = get(port, "/api/stats?token=" + token);
  CHECK_EQ(stats.status, 200);
  CHECK(stats.body.find("\"produced\":") != std::string::npos);
  CHECK(stats.body.find("\"crash_handler\":true") != std::string::npos);

  const auto unauthorized = get(port, "/api/snapshot");
  CHECK_EQ(unauthorized.status, 401);

  const auto root = get(port, "/?token=" + token);
  CHECK_EQ(root.status, 200);
  CHECK(root.body.find("<!doctype html>") != std::string::npos);
  CHECK(root.headers.find("Set-Cookie: bb_token=") != std::string::npos);

  const auto js = get(port, "/app.js?token=" + token);
  CHECK_EQ(js.status, 200);
  CHECK(js.body.find("Blackbox") != std::string::npos);

  const auto snapshot = get(port, "/api/snapshot?token=" + token + "&limit=100");
  CHECK_EQ(snapshot.status, 200);
  CHECK(snapshot.body.find("\"ok\":true") != std::string::npos);
  CHECK(snapshot.body.find("hello from test") != std::string::npos);
  CHECK(snapshot.body.find("\"graph\":") != std::string::npos);
  CHECK(snapshot.body.find("\"waterfall\":") != std::string::npos);
  CHECK(snapshot.body.find("\"failure\":") != std::string::npos);
  CHECK(snapshot.body.find("\"analysis_error\":\"\"") != std::string::npos);

  const auto via_header = get(port, "/api/snapshot",
                              "X-Blackbox-Token: " + token + "\r\n");
  CHECK_EQ(via_header.status, 200);
  const auto via_cookie = get(port, "/api/snapshot",
                              "Cookie: bb_token=" + token + "\r\n");
  CHECK_EQ(via_cookie.status, 200);
  const auto via_bearer = get(port, "/api/snapshot",
                              "Authorization: Bearer " + token + "\r\n");
  CHECK_EQ(via_bearer.status, 200);

  const auto filtered =
      get(port, "/api/snapshot?token=" + token + "&q=kind%3Derror");
  CHECK_EQ(filtered.status, 200);
  CHECK(filtered.body.find("\"message\":\"boom\"") != std::string::npos);
  CHECK(filtered.body.find("\"message\":\"hello from test\"") == std::string::npos);

  const auto bad_query = get(port, "/api/snapshot?token=" + token + "&q=kind%20%3D");
  CHECK_EQ(bad_query.status, 400);

  const auto missing = get(port, "/nope?token=" + token);
  CHECK_EQ(missing.status, 404);

  const auto explained = get(port, "/api/explain?seq=5&token=" + token);
  CHECK_EQ(explained.status, 200);
  CHECK(explained.body.find("\"found\":true") != std::string::npos);
  CHECK(explained.body.find("\"chain\":[") != std::string::npos);
  CHECK(explained.body.find("database") != std::string::npos ||
        explained.body.find("root_seq") != std::string::npos);
  const auto missing_explain = get(port, "/api/explain?seq=99999&token=" + token);
  CHECK_EQ(missing_explain.status, 404);
  const auto bad_explain = get(port, "/api/explain?seq=nope&token=" + token);
  CHECK_EQ(bad_explain.status, 400);

  const auto malformed = request(port, "GARBAGE\r\n\r\n");
  CHECK_EQ(malformed.status, 400);

  const auto bad_host =
      request(port, "GET /api/health HTTP/1.1\r\nHost: attacker.example\r\n\r\n");
  CHECK_EQ(bad_host.status, 403);
  const auto missing_host = request(port, "GET /api/health HTTP/1.1\r\n\r\n");
  CHECK_EQ(missing_host.status, 403);
  const auto localhost_host =
      request(port, "GET /api/health HTTP/1.1\r\nHost: localhost:" +
                        std::to_string(port) + "\r\n\r\n");
  CHECK_EQ(localhost_host.status, 200);

  server.stop();
  CHECK(!server.running());
}

BB_TEST(server_dump_browser_and_flush) {
  bbtest::TempDir temp;
  bb::RecorderConfig config;
  config.ring_capacity = 64;
  config.dump_directory = temp.file("dumps");
  bb::Recorder recorder(config);
  for (int i = 0; i < 6; ++i) {
    CHECK(recorder.log(bb::Level::Info, "dump log " + std::to_string(i)));
  }
  bb::UiServer server(recorder);
  std::string error;
  CHECK(server.start(&error));
  const std::uint16_t port = server.port();
  const std::string& token = server.token();

  const std::string post =
      "POST /api/dump HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Blackbox-Token: " +
      token + "\r\nContent-Length: 0\r\n\r\n";
  const auto flushed = request(port, post);
  CHECK_EQ(flushed.status, 200);
  CHECK(flushed.body.find("\"ok\":true") != std::string::npos);

  const std::string no_header =
      "POST /api/dump HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
  const auto rejected = request(port, no_header);
  CHECK_EQ(rejected.status, 401);

  const auto dumps = get(port, "/api/dumps?token=" + token);
  CHECK_EQ(dumps.status, 200);
  CHECK(dumps.body.find("\"dumps\":[") != std::string::npos);
  CHECK(dumps.body.find("\"valid\":true") != std::string::npos);
  CHECK(dumps.body.find("\"complete\":true") != std::string::npos);
  CHECK(dumps.body.find(".bbx") != std::string::npos);

  const std::size_t name_start = dumps.body.find("\"name\":\"");
  CHECK(name_start != std::string::npos);
  if (name_start == std::string::npos) {
    return;
  }
  const std::size_t value_start = name_start + 8;
  const std::size_t value_end = dumps.body.find('"', value_start);
  CHECK(value_end != std::string::npos);
  const std::string name = dumps.body.substr(value_start, value_end - value_start);
  CHECK(name.find(".bbx") != std::string::npos);

  const auto download = get(port, "/api/dump?name=" + name + "&token=" + token);
  CHECK_EQ(download.status, 200);
  CHECK(download.body.rfind("BBXDMP01", 0) == 0);

  const auto opened = get(port, "/api/dump/open?name=" + name + "&token=" + token);
  CHECK_EQ(opened.status, 200);
  CHECK(opened.body.find("\"dump\":{") != std::string::npos);
  CHECK(opened.body.find("\"record_count\":6") != std::string::npos);
  CHECK(opened.body.find("dump log 3") != std::string::npos);

  CHECK(recorder.log(bb::Level::Warn, "diverging record"));
  const auto second_flush = request(port, post);
  CHECK_EQ(second_flush.status, 200);
  const auto all_dumps = recorder.list_dumps(10);
  CHECK_EQ(all_dumps.size(), 2u);
  std::string first_name;
  std::string second_name;
  for (const auto& dump : all_dumps) {
    if (dump.meta.record_count == 6) first_name = dump.name;
    if (dump.meta.record_count == 7) second_name = dump.name;
  }
  CHECK(!first_name.empty());
  CHECK(!second_name.empty());
  const auto compared = get(port, "/api/compare?left=" + first_name +
                                   "&right=" + second_name + "&token=" + token);
  CHECK_EQ(compared.status, 200);
  CHECK(compared.body.find("\"event_divergence\":true") != std::string::npos);
  CHECK(compared.body.find("\"event_index\":6") != std::string::npos);
  CHECK(compared.body.find("\"left\":{") != std::string::npos);

  const auto missing_compare = get(port, "/api/compare?left=nope.bbx&right=" +
                                          second_name + "&token=" + token);
  CHECK_EQ(missing_compare.status, 404);

  server.stop();
}

BB_TEST(server_streams_sse_snapshot_events) {
  bb::Recorder recorder(bb::RecorderConfig{32});
  CHECK(recorder.log(bb::Level::Info, "streamed event"));
  bb::UiServer server(recorder);
  std::string error;
  CHECK(server.start(&error));
  const int fd = connect_local(server.port());
  CHECK(fd >= 0);
  if (fd < 0) {
    return;
  }
  const std::string request_text =
      "GET /api/stream?token=" + server.token() +
      " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  CHECK(send_string(fd, request_text));
  std::string data;
  std::array<char, 4096> buffer{};
  for (int i = 0; i < 200 && data.find("event: snapshot") == std::string::npos;
       ++i) {
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (got <= 0) {
      break;
    }
    data.append(buffer.data(), static_cast<std::size_t>(got));
    if (data.size() > 4u * 1024u * 1024u) {
      break;
    }
  }
  ::close(fd);
  CHECK(data.find("text/event-stream") != std::string::npos);
  CHECK(data.find("event: snapshot") != std::string::npos);
  CHECK(data.find("\"ok\":true") != std::string::npos);
  CHECK(data.find("streamed event") != std::string::npos);
  server.stop();
}

BB_TEST(server_rejects_too_many_streams_and_oversized_limits) {
  bb::Recorder recorder(bb::RecorderConfig{16});
  bb::UiServer server(recorder);
  std::string error;
  CHECK(server.start(&error));
  const auto bad_limit = get(
      server.port(), "/api/snapshot?token=" + server.token() + "&limit=999999");
  CHECK_EQ(bad_limit.status, 400);
  const auto bad_minutes = get(
      server.port(), "/api/snapshot?token=" + server.token() + "&minutes=99999");
  CHECK_EQ(bad_minutes.status, 400);
  server.stop();
}
