#include "http.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace bb::ui {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool parse_params(std::string_view query, Request& request, std::string& error) {
  std::size_t count = 0;
  while (!query.empty()) {
    const std::size_t ampersand = query.find('&');
    const std::string_view pair =
        ampersand == std::string_view::npos ? query : query.substr(0, ampersand);
    query = ampersand == std::string_view::npos
                ? std::string_view()
                : query.substr(ampersand + 1);
    if (pair.empty()) {
      continue;
    }
    if (++count > kMaxParams) {
      error = "too many query parameters";
      return false;
    }
    const std::size_t equals = pair.find('=');
    const std::string_view raw_key =
        equals == std::string_view::npos ? pair : pair.substr(0, equals);
    const std::string_view raw_value =
        equals == std::string_view::npos ? std::string_view()
                                         : pair.substr(equals + 1);
    if (raw_key.empty() || raw_key.size() > kMaxParamBytes ||
        raw_value.size() > kMaxParamBytes) {
      error = "query parameter is too large";
      return false;
    }
    std::string key;
    std::string value;
    if (!url_decode(raw_key, false, key) ||
        !url_decode(raw_value, true, value)) {
      error = "malformed query escape";
      return false;
    }
    if (key.empty() || key.size() > 128) {
      error = "invalid query parameter name";
      return false;
    }
    request.params.try_emplace(std::move(key), std::move(value));
  }
  return true;
}

bool parse_request_line(std::string_view line, Request& request,
                        std::string& error) {
  const std::size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    error = "malformed request line";
    return false;
  }
  const std::size_t second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) {
    error = "malformed request line";
    return false;
  }
  if (line.find(' ', second_space + 1) != std::string_view::npos) {
    error = "malformed request line";
    return false;
  }
  request.method = std::string(line.substr(0, first_space));
  request.target = std::string(
      line.substr(first_space + 1, second_space - first_space - 1));
  request.version = std::string(line.substr(second_space + 1));
  if (request.method != "GET" && request.method != "POST" &&
      request.method != "HEAD") {
    error = "unsupported method";
    return false;
  }
  if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
    error = "unsupported HTTP version";
    return false;
  }
  if (request.target.empty() || request.target.size() > kMaxPathBytes ||
      request.target.front() != '/') {
    error = "invalid request target";
    return false;
  }
  const std::size_t question = request.target.find('?');
  const std::string_view raw_path =
      question == std::string::npos ? std::string_view(request.target)
                                    : std::string_view(request.target)
                                          .substr(0, question);
  if (!url_decode(raw_path, false, request.path)) {
    error = "malformed path escape";
    return false;
  }
  if (request.path.empty() || request.path.front() != '/') {
    error = "invalid decoded path";
    return false;
  }
  if (question != std::string::npos) {
    const std::string_view query =
        std::string_view(request.target).substr(question + 1);
    if (!parse_params(query, request, error)) {
      return false;
    }
  }
  return true;
}

}  // namespace

const std::string* Request::header(std::string_view name) const {
  const auto found = headers.find(std::string(name));
  return found == headers.end() ? nullptr : &found->second;
}

const std::string* Request::param(std::string_view name) const {
  const auto found = params.find(std::string(name));
  return found == params.end() ? nullptr : &found->second;
}

bool url_decode(std::string_view text, bool plus_is_space, std::string& out) {
  out.clear();
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      if (i + 2 >= text.size()) {
        return false;
      }
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high < 0 || low < 0) {
        return false;
      }
      const char decoded = static_cast<char>((high << 4) | low);
      if (decoded == '\0') {
        return false;
      }
      out.push_back(decoded);
      i += 2;
    } else if (c == '+' && plus_is_space) {
      out.push_back(' ');
    } else if (c == '\0') {
      return false;
    } else {
      out.push_back(c);
    }
  }
  return true;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

bool read_request(int fd, Request& request, std::string& error) {
  std::string buffer;
  buffer.reserve(1024);
  std::array<char, 2048> chunk{};
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = "socket read failed";
      return false;
    }
    if (got == 0) {
      error = "connection closed before request headers";
      return false;
    }
    if (buffer.size() + static_cast<std::size_t>(got) > kMaxHeaderBytes) {
      error = "request headers exceed the size limit";
      return false;
    }
    buffer.append(chunk.data(), static_cast<std::size_t>(got));
    header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      header_end = buffer.find("\n\n");
    }
  }
  std::string_view head(buffer.data(), header_end);
  std::size_t line_start = 0;
  bool first = true;
  std::size_t header_count = 0;
  while (line_start <= head.size()) {
    std::size_t line_end = head.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = head.size();
    }
    std::string_view line = head.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (first) {
      if (!parse_request_line(line, request, error)) {
        return false;
      }
      first = false;
    } else if (!line.empty()) {
      if (++header_count > 64) {
        error = "too many headers";
        return false;
      }
      if (line.front() == ' ' || line.front() == '\t') {
        error = "folded headers are not allowed";
        return false;
      }
      const std::size_t colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        error = "malformed header";
        return false;
      }
      std::string name = std::string(trim(line.substr(0, colon)));
      const std::string_view value = trim(line.substr(colon + 1));
      if (name.empty() || name.size() > 128 || value.size() > kMaxParamBytes) {
        error = "header is too large";
        return false;
      }
      for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c - 'A' + 'a');
        }
        if (c < 0x21 || c > 0x7E) {
          error = "invalid header name";
          return false;
        }
      }
      request.headers.try_emplace(std::move(name), std::string(value));
    }
    if (line_end == head.size()) {
      break;
    }
    line_start = line_end + 1;
  }
  if (first) {
    error = "empty request";
    return false;
  }
  if (const std::string* length = request.header("content-length")) {
    std::uint64_t value = 0;
    if (!parse_u64(*length, value) || value > kMaxBodyBytes) {
      error = "request body is too large";
      return false;
    }
  }
  return true;
}

bool send_all(int fd, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const char*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t sent = ::send(fd, bytes + offset, size - offset, 0);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (sent == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

namespace {

const char* status_text(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Payload Too Large";
    case 429:
      return "Too Many Requests";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "OK";
  }
}

}  // namespace

bool send_response(int fd, const Response& response, bool head_only) {
  std::string head;
  head.reserve(256 + response.extra_headers.size() * 64);
  head += "HTTP/1.1 ";
  head += std::to_string(response.status);
  head += ' ';
  head += status_text(response.status);
  head += "\r\nContent-Type: ";
  head += response.content_type;
  head += "\r\nContent-Length: ";
  head += std::to_string(response.body.size());
  head += "\r\nCache-Control: ";
  head += response.cacheable ? "max-age=60" : "no-store";
  head += "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n";
  head += "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
  for (const auto& extra : response.extra_headers) {
    head += extra.first;
    head += ": ";
    head += extra.second;
    head += "\r\n";
  }
  head += "\r\n";
  if (!send_all(fd, head.data(), head.size())) {
    return false;
  }
  if (!head_only && !response.body.empty()) {
    return send_all(fd, response.body.data(), response.body.size());
  }
  return true;
}

}  // namespace bb::ui
