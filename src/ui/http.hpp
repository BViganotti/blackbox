#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bb::ui {

inline constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
inline constexpr std::size_t kMaxPathBytes = 2048;
inline constexpr std::size_t kMaxParams = 32;
inline constexpr std::size_t kMaxParamBytes = 2048;
inline constexpr std::size_t kMaxBodyBytes = 4096;

struct Request {
  std::string method;
  std::string target;
  std::string path;
  std::string version;
  std::unordered_map<std::string, std::string> params;
  std::unordered_map<std::string, std::string> headers;

  const std::string* header(std::string_view name) const;
  const std::string* param(std::string_view name) const;
};

struct Response {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> extra_headers;
  bool cacheable = false;
};

/// Reads and parses one HTTP/1.x request from a connected socket. The socket
/// must have a receive timeout. Bounds every component and rejects NUL bytes,
/// folded headers, oversized bodies and malformed percent escapes.
bool read_request(int fd, Request& request, std::string& error);

/// Serialises and writes a response. `head_only` suppresses the body while
/// retaining Content-Length. Returns false on a socket error.
bool send_response(int fd, const Response& response, bool head_only = false);

bool send_all(int fd, const void* data, std::size_t size);

/// Decodes percent escapes. `plus_is_space` is used for query strings. Returns
/// false on a malformed escape or an embedded NUL.
bool url_decode(std::string_view text, bool plus_is_space, std::string& out);

/// Parses a bounded unsigned decimal. Returns false for empty, non-numeric or
/// overflowing values.
bool parse_u64(std::string_view text, std::uint64_t& out);

}  // namespace bb::ui
