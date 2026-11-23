#pragma once

#include <blackbox/recorder.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bb {

/// Local browser workbench server. Binds an ephemeral port on 127.0.0.1, serves
/// the compiled-in UI assets, JSON snapshots and server-sent events. Every
/// request except /api/health requires the random session token (query, cookie,
/// bearer header or X-Blackbox-Token).
///
/// start() and stop() are not reentrant; call them from one thread. Request
/// handling itself is concurrent. The Recorder must outlive the server.
class UiServer {
 public:
  /// `demo_trigger` is optional; when set, POST /api/demo/trigger invokes it.
  explicit UiServer(Recorder& recorder,
                    std::function<void()> demo_trigger = {});
  ~UiServer();
  UiServer(const UiServer&) = delete;
  UiServer& operator=(const UiServer&) = delete;

  /// Starts listening and the worker pool. Returns false and fills `error` on
  /// bind/listen/thread failure. Calling start twice is a no-op returning true.
  bool start(std::string* error = nullptr);

  /// Stops accepting, closes client sockets and joins all threads.
  void stop();

  bool running() const;
  std::uint16_t port() const;
  const std::string& token() const;

  /// http://127.0.0.1:<port>/?token=<token>
  std::string url() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Opens `url` in the system browser without a shell. macOS uses /usr/bin/open,
/// other POSIX systems try xdg-open. Returns false if the launcher could not be
/// started or exited non-zero.
bool open_browser(std::string_view url, std::string* error = nullptr);

}  // namespace bb
