#include <blackbox/ui_server.hpp>

#include "http.hpp"

#include <blackbox/assets.hpp>
#include <blackbox/causal.hpp>
#include <blackbox/dump.hpp>
#include <blackbox/json.hpp>
#include <blackbox/present.hpp>
#include <blackbox/query.hpp>
#include <blackbox/util/hex.hpp>
#include <blackbox/util/random.hpp>
#include <blackbox/util/time.hpp>
#include <blackbox/version.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bb {

using ui::Request;
using ui::Response;

namespace {

constexpr std::size_t kWorkerThreads = 12;
constexpr std::size_t kMaxQueue = 32;
constexpr std::size_t kMaxSseConnections = 4;
constexpr std::uint64_t kDefaultLimit = 1200;
constexpr std::uint64_t kMaxLimit = 4000;
constexpr int kDefaultMinutes = 15;
constexpr int kMaxMinutes = 1440;
constexpr std::size_t kMaxDownloadBytes = 64u * 1024u * 1024u;

void configure_socket(int fd) {
  struct timeval timeout {};
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

std::string node_label(const Record& record) {
  if (!record_name(record).empty()) {
    return std::string(record_name(record));
  }
  if (!record_state_key(record).empty()) {
    return std::string(record_state_key(record));
  }
  if (!record_message(record).empty()) {
    std::string text(record_message(record));
    if (text.size() > 96) {
      text.resize(96);
      text += "...";
    }
    return text;
  }
  return to_string(record.kind);
}

std::string safe_filename(std::string_view name) {
  std::string out;
  for (char c : name) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '-';
    out.push_back(safe ? c : '_');
    if (out.size() >= 128) {
      break;
    }
  }
  return out.empty() ? "dump.bbx" : out;
}

std::string cookie_value(const Request& request, std::string_view name) {
  const std::string* cookie = request.header("cookie");
  if (cookie == nullptr) {
    return {};
  }
  std::string_view text(*cookie);
  std::size_t count = 0;
  while (!text.empty() && ++count <= 64) {
    const std::size_t semicolon = text.find(';');
    std::string_view part =
        semicolon == std::string_view::npos ? text : text.substr(0, semicolon);
    text = semicolon == std::string_view::npos ? std::string_view()
                                               : text.substr(semicolon + 1);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
      part.remove_prefix(1);
    }
    const std::size_t equals = part.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    if (part.substr(0, equals) == name) {
      return std::string(part.substr(equals + 1));
    }
  }
  return {};
}

bool read_file_bounded(const std::string& path, std::size_t max_bytes,
                       std::string& out, std::string& error) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = "cannot open dump file";
    return false;
  }
  struct stat status {};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > max_bytes) {
    error = "dump file is unavailable or too large";
    ::close(fd);
    return false;
  }
  out.resize(static_cast<std::size_t>(status.st_size));
  std::size_t offset = 0;
  while (offset < out.size()) {
    const ssize_t got = ::read(fd, out.data() + offset, out.size() - offset);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = "dump read failed";
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    offset += static_cast<std::size_t>(got);
  }
  ::close(fd);
  out.resize(offset);
  return true;
}

struct ViewParams {
  std::uint64_t limit = kDefaultLimit;
  int minutes = kDefaultMinutes;
  std::string query;
};

bool parse_view_params(const Request& request, std::uint64_t default_limit,
                       ViewParams& params, std::string& error) {
  params.limit = default_limit;
  if (const std::string* limit = request.param("limit")) {
    std::uint64_t value = 0;
    if (!ui::parse_u64(*limit, value) || value == 0 || value > kMaxLimit) {
      error = "limit must be between 1 and 4000";
      return false;
    }
    params.limit = value;
  }
  if (const std::string* minutes = request.param("minutes")) {
    std::uint64_t value = 0;
    if (!ui::parse_u64(*minutes, value) || value > kMaxMinutes) {
      error = "minutes must be between 0 and 1440";
      return false;
    }
    params.minutes = static_cast<int>(value);
  }
  if (const std::string* query = request.param("q")) {
    params.query = *query;
  } else if (const std::string* named_query = request.param("query")) {
    params.query = *named_query;
  }
  if (params.query.size() > 4096) {
    error = "query is too large";
    return false;
  }
  return true;
}

void write_graph_json(JsonWriter& out, const CausalGraph& graph,
                      const GraphLayout& layout) {
  out.begin_object();
  out.key("layers");
  out.number(static_cast<std::uint64_t>(layout.layers));
  out.key("widest_layer");
  out.number(static_cast<std::uint64_t>(layout.widest_layer));
  out.key("unresolved_parents");
  out.number(graph.unresolved_parents);
  out.key("roots");
  out.begin_array();
  for (std::uint32_t root : graph.roots) {
    out.number(static_cast<std::uint64_t>(root));
  }
  out.end_array();
  out.key("nodes");
  out.begin_array();
  for (std::size_t i = 0; i < graph.records.size(); ++i) {
    const Record& record = graph.records[i];
    out.begin_object();
    out.key("id");
    out.number(static_cast<std::uint64_t>(i));
    out.key("seq");
    out.number(record.seq);
    out.key("kind");
    out.string(to_string(record.kind));
    out.key("level");
    out.string(to_string(record.level));
    out.key("status");
    out.string(to_string(record.status));
    out.key("name");
    out.string(record_name(record));
    out.key("label");
    out.string(node_label(record));
    out.key("message");
    out.string(record_message(record));
    out.key("state_key");
    out.string(record_state_key(record));
    out.key("trace");
    out.string(record.trace.to_hex());
    out.key("span");
    out.number(record.span);
    out.key("parent");
    out.number(record.parent);
    out.key("correlation");
    out.number(record.correlation);
    out.key("time");
    if (record.wall_ns > 0) {
      out.string(format_wall_ns(record.wall_ns));
    } else {
      out.null();
    }
    const GraphPoint point =
        i < layout.points.size() ? layout.points[i] : GraphPoint{};
    out.key("layer");
    out.number(static_cast<std::uint64_t>(point.layer));
    out.key("row");
    out.number(static_cast<std::uint64_t>(point.row));
    out.key("x");
    out.number(static_cast<double>(point.x));
    out.key("y");
    out.number(static_cast<double>(point.y));
    out.end_object();
  }
  out.end_array();
  out.key("edges");
  out.begin_array();
  for (const CausalEdge& edge : graph.edges) {
    out.begin_object();
    out.key("from");
    out.number(static_cast<std::uint64_t>(edge.from));
    out.key("to");
    out.number(static_cast<std::uint64_t>(edge.to));
    out.key("kind");
    out.string(to_string(edge.kind));
    out.end_object();
  }
  out.end_array();
  out.end_object();
}

void write_waterfall_json(JsonWriter& out, const CausalGraph& graph) {
  out.begin_array();
  for (const SpanRow& row : span_waterfall(graph, 2000)) {
    out.begin_object();
    out.key("trace");
    out.string(row.trace.to_hex());
    out.key("span");
    out.number(row.span);
    out.key("parent");
    out.number(row.parent);
    out.key("name");
    out.string(row.name);
    out.key("status");
    out.string(to_string(row.status));
    out.key("start_mono_ns");
    out.number(row.start_mono_ns);
    out.key("duration_ns");
    out.number(row.duration_ns);
    out.key("depth");
    out.number(static_cast<std::uint64_t>(row.depth));
    out.key("begin_node");
    out.number(static_cast<std::uint64_t>(row.begin_node));
    out.key("end_node");
    if (row.end_node == kInvalidNode) {
      out.null();
    } else {
      out.number(static_cast<std::uint64_t>(row.end_node));
    }
    out.end_object();
  }
  out.end_array();
}

void write_states_json(JsonWriter& out, const CausalGraph& graph) {
  out.begin_array();
  for (const StateHistory& history : state_timeline(graph, 64, 256)) {
    out.begin_object();
    out.key("key");
    out.string(history.key);
    out.key("transitions");
    out.begin_array();
    for (const StateTransition& transition : history.transitions) {
      out.begin_object();
      out.key("seq");
      out.number(transition.seq);
      out.key("wall_ns");
      out.number(transition.wall_ns);
      out.key("time");
      if (transition.wall_ns > 0) {
        out.string(format_wall_ns(transition.wall_ns));
      } else {
        out.null();
      }
      out.key("node");
      out.number(static_cast<std::uint64_t>(transition.node));
      out.key("previous");
      write_value_json(out, transition.previous);
      out.key("next");
      write_value_json(out, transition.next);
      out.end_object();
    }
    out.end_array();
    out.end_object();
  }
  out.end_array();
}

void write_failure_json(JsonWriter& out, const FailureExplanation& failure) {
  out.begin_object();
  out.key("found");
  out.boolean(failure.found);
  out.key("error_node");
  if (failure.error_node == kInvalidNode) {
    out.null();
  } else {
    out.number(static_cast<std::uint64_t>(failure.error_node));
  }
  out.key("root_cause");
  if (failure.root_cause == kInvalidNode) {
    out.null();
  } else {
    out.number(static_cast<std::uint64_t>(failure.root_cause));
  }
  out.key("chain");
  out.begin_array();
  for (std::uint32_t node : failure.chain) {
    out.number(static_cast<std::uint64_t>(node));
  }
  out.end_array();
  out.key("findings");
  out.begin_array();
  for (const std::string& finding : failure.findings) {
    out.string(finding);
  }
  out.end_array();
  out.key("summary");
  out.string(failure.summary);
  out.end_object();
}

bool write_analysis(JsonWriter& out, const std::vector<Record>& records,
                    std::string& analysis_error) {
  analysis_error.clear();
  CausalGraph graph = build_causal_graph(records, &analysis_error);
  const GraphLayout layout = layout_causal_graph(graph);
  out.key("records");
  out.begin_array();
  for (const Record& record : records) {
    write_record_json(out, record);
  }
  out.end_array();
  out.key("graph");
  write_graph_json(out, graph, layout);
  out.key("waterfall");
  write_waterfall_json(out, graph);
  out.key("states");
  write_states_json(out, graph);
  out.key("failure");
  write_failure_json(out, explain_latest_failure(graph));
  out.key("analysis_error");
  out.string(analysis_error);
  return out.ok();
}

Response json_error(int status, std::string_view message) {
  Response response;
  response.status = status;
  JsonWriter out;
  out.begin_object();
  out.key("ok");
  out.boolean(false);
  out.key("error");
  out.string(message);
  out.end_object();
  if (out.ok()) {
    response.body = out.str();
  } else {
    response.content_type = "text/plain; charset=utf-8";
    response.body = "internal error";
  }
  return response;
}

}  // namespace

struct UiServer::Impl {
  Impl(Recorder& recorder_in, std::function<void()> trigger_in)
      : recorder(recorder_in), demo_trigger(std::move(trigger_in)) {
    token = random_token(16);
  }

  Recorder& recorder;
  std::function<void()> demo_trigger;
  std::string token;
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::atomic<bool> running{false};
  bool started = false;
  std::thread accept_thread;
  std::vector<std::thread> workers;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<int> queue;
  std::mutex clients_mutex;
  std::unordered_set<int> clients;
  std::atomic<int> active_sse{0};
  std::int64_t started_wall_ns = 0;

  bool start(std::string* error);
  void stop();
  void accept_loop();
  void worker_loop();
  void handle_connection(int fd);
  Response route(const Request& request);
  bool authorized(const Request& request) const;
  bool token_matches(const std::string& candidate) const;
  bool require_post_token(const Request& request) const;
  Response serve_root(const Request& request);
  Response serve_asset(std::string_view path);
  Response serve_snapshot(const Request& request, std::uint64_t default_limit);
  Response serve_dumps();
  Response serve_download(const Request& request);
  Response serve_open(const Request& request);
  Response serve_flush();
  void serve_sse(int fd, const Request& request);
  bool build_snapshot(const Request& request, std::uint64_t default_limit,
                      std::string& body, int& status, std::string& error);
  void close_client(int fd);
};

bool UiServer::Impl::token_matches(const std::string& candidate) const {
  return !candidate.empty() && secure_equal(candidate, token);
}

bool UiServer::Impl::authorized(const Request& request) const {
  if (const std::string* query = request.param("token");
      query != nullptr && token_matches(*query)) {
    return true;
  }
  if (const std::string* header = request.header("x-blackbox-token");
      header != nullptr && token_matches(*header)) {
    return true;
  }
  if (const std::string* header = request.header("authorization");
      header != nullptr && header->size() > 7 &&
      header->compare(0, 7, "Bearer ") == 0 &&
      token_matches(header->substr(7))) {
    return true;
  }
  const std::string cookie = cookie_value(request, "bb_token");
  return token_matches(cookie);
}

bool UiServer::Impl::require_post_token(const Request& request) const {
  const std::string* header = request.header("x-blackbox-token");
  return header != nullptr && token_matches(*header);
}

bool UiServer::Impl::start(std::string* error) {
  std::string local_error;
  std::string& err = error != nullptr ? *error : local_error;
  err.clear();
  if (started) {
    return true;
  }
  std::signal(SIGPIPE, SIG_IGN);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = std::string("cannot create socket: ") + std::strerror(errno);
    return false;
  }
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#if defined(SO_NOSIGPIPE)
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address),
             sizeof(address)) != 0) {
    err = std::string("cannot bind loopback socket: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (::listen(fd, 32) != 0) {
    err = std::string("cannot listen: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address),
                    &length) != 0) {
    err = std::string("cannot read bound port: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  listen_fd = fd;
  port = ntohs(address.sin_port);
  started_wall_ns = now_wall_ns();
  running.store(true, std::memory_order_release);
  started = true;
  try {
    accept_thread = std::thread([this] { accept_loop(); });
    workers.reserve(kWorkerThreads);
    for (std::size_t i = 0; i < kWorkerThreads; ++i) {
      workers.emplace_back([this] { worker_loop(); });
    }
  } catch (const std::exception& exception) {
    err = std::string("cannot start server threads: ") + exception.what();
    stop();
    return false;
  }
  return true;
}

void UiServer::Impl::close_client(int fd) {
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
}

void UiServer::Impl::stop() {
  if (!started) {
    return;
  }
  started = false;
  running.store(false, std::memory_order_release);
  if (listen_fd >= 0) {
    ::shutdown(listen_fd, SHUT_RDWR);
    ::close(listen_fd);
    listen_fd = -1;
  }
  {
    std::lock_guard<std::mutex> guard(clients_mutex);
    for (int fd : clients) {
      ::shutdown(fd, SHUT_RDWR);
    }
  }
  queue_cv.notify_all();
  if (accept_thread.joinable()) {
    accept_thread.join();
  }
  for (std::thread& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers.clear();
  {
    std::lock_guard<std::mutex> guard(queue_mutex);
    for (int fd : queue) {
      ::close(fd);
    }
    queue.clear();
  }
  {
    std::lock_guard<std::mutex> guard(clients_mutex);
    clients.clear();
  }
  port = 0;
}

void UiServer::Impl::accept_loop() {
  while (running.load(std::memory_order_acquire)) {
    struct pollfd descriptor {};
    descriptor.fd = listen_fd;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, 200);
    if (ready <= 0) {
      continue;
    }
    if ((descriptor.revents & POLLIN) == 0) {
      continue;
    }
    struct sockaddr_in client_address {};
    socklen_t client_length = sizeof(client_address);
    const int client = ::accept(
        listen_fd, reinterpret_cast<struct sockaddr*>(&client_address),
        &client_length);
    if (client < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      if (!running.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    configure_socket(client);
    {
      std::lock_guard<std::mutex> guard(clients_mutex);
      clients.insert(client);
    }
    bool full = false;
    {
      std::lock_guard<std::mutex> guard(queue_mutex);
      if (queue.size() >= kMaxQueue) {
        full = true;
      } else {
        queue.push_back(client);
        queue_cv.notify_one();
      }
    }
    if (full) {
      static constexpr char kBusy[] =
          "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      (void)::send(client, kBusy, sizeof(kBusy) - 1, 0);
      ::close(client);
      std::lock_guard<std::mutex> clients_guard(clients_mutex);
      clients.erase(client);
      continue;
    }
  }
}

void UiServer::Impl::worker_loop() {
  for (;;) {
    int fd = -1;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_cv.wait(lock, [this] {
        return !running.load(std::memory_order_acquire) || !queue.empty();
      });
      if (!running.load(std::memory_order_acquire) && queue.empty()) {
        return;
      }
      if (queue.empty()) {
        continue;
      }
      fd = queue.front();
      queue.pop_front();
    }
    if (!running.load(std::memory_order_acquire)) {
      ::close(fd);
      std::lock_guard<std::mutex> guard(clients_mutex);
      clients.erase(fd);
      continue;
    }
    handle_connection(fd);
    {
      std::lock_guard<std::mutex> guard(clients_mutex);
      clients.erase(fd);
    }
    ::close(fd);
  }
}

void UiServer::Impl::handle_connection(int fd) {
  Request request;
  std::string error;
  if (!ui::read_request(fd, request, error)) {
    if (!error.empty() && error.find("closed") == std::string::npos) {
      (void)ui::send_response(fd, json_error(400, error));
    }
    return;
  }
  if (request.path == "/api/stream" && request.method == "GET") {
    serve_sse(fd, request);
    return;
  }
  Response response = route(request);
  (void)ui::send_response(fd, response, request.method == "HEAD");
}

Response UiServer::Impl::serve_root(const Request& request) {
  Response response;
  const bb::assets::Asset* asset = bb::assets::find("/index.html");
  if (asset == nullptr) {
    return json_error(500, "index asset missing");
  }
  response.content_type = std::string(asset->content_type);
  response.body.assign(reinterpret_cast<const char*>(asset->bytes.data()),
                       asset->bytes.size());
  if (const std::string* query = request.param("token");
      query != nullptr && token_matches(*query)) {
    response.extra_headers.emplace_back(
        "Set-Cookie", "bb_token=" + token + "; Path=/; SameSite=Strict");
  }
  return response;
}

Response UiServer::Impl::serve_asset(std::string_view path) {
  Response response;
  const bb::assets::Asset* asset = bb::assets::find(path);
  if (asset == nullptr) {
    return json_error(404, "asset not found");
  }
  response.content_type = std::string(asset->content_type);
  response.body.assign(reinterpret_cast<const char*>(asset->bytes.data()),
                       asset->bytes.size());
  response.cacheable = true;
  return response;
}

bool UiServer::Impl::build_snapshot(const Request& request,
                                    std::uint64_t default_limit,
                                    std::string& body, int& status,
                                    std::string& error) {
  status = 200;
  error.clear();
  ViewParams params;
  if (!parse_view_params(request, default_limit, params, error)) {
    status = 400;
    return false;
  }
  std::optional<Query> query;
  if (!params.query.empty()) {
    query = Query::parse(params.query, &error);
    if (!query.has_value()) {
      status = 400;
      return false;
    }
  }
  RecorderSnapshot snapshot =
      recorder.snapshot(static_cast<std::size_t>(params.limit));
  const std::uint64_t snapshot_first = snapshot.first_seq;
  const std::uint64_t snapshot_last = snapshot.last_seq;
  const std::uint64_t snapshot_lost = snapshot.lost;
  std::vector<Record> records = std::move(snapshot.records);
  if (params.minutes > 0) {
    const std::int64_t cutoff =
        now_wall_ns() -
        static_cast<std::int64_t>(params.minutes) * 60'000'000'000ll;
    std::erase_if(records, [cutoff](const Record& record) {
      return record.wall_ns > 0 && record.wall_ns < cutoff;
    });
  }
  if (query.has_value()) {
    std::erase_if(records, [&query](const Record& record) {
      return !query->matches(record);
    });
  }
  JsonWriter out;
  out.begin_object();
  out.key("ok");
  out.boolean(true);
  out.key("generated_wall_ns");
  out.number(now_wall_ns());
  out.key("product");
  out.string("blackbox");
  out.key("version");
  out.string(kVersion);
  out.key("stats");
  write_recorder_stats_json(out, recorder.stats());
  out.key("window");
  out.begin_object();
  out.key("requested_limit");
  out.number(params.limit);
  out.key("minutes");
  out.number(static_cast<std::int64_t>(params.minutes));
  out.key("returned");
  out.number(static_cast<std::uint64_t>(records.size()));
  out.key("snapshot_first_seq");
  out.number(snapshot_first);
  out.key("snapshot_last_seq");
  out.number(snapshot_last);
  out.key("snapshot_lost");
  out.number(snapshot_lost);
  out.end_object();
  std::string analysis_error;
  if (!write_analysis(out, records, analysis_error)) {
    status = 500;
    error = "snapshot response exceeds the output limit";
    return false;
  }
  out.end_object();
  if (!out.ok()) {
    status = 500;
    error = "snapshot response exceeds the output limit";
    return false;
  }
  body = out.str();
  return true;
}

Response UiServer::Impl::serve_snapshot(const Request& request,
                                        std::uint64_t default_limit) {
  std::string body;
  std::string error;
  int status = 200;
  if (!build_snapshot(request, default_limit, body, status, error)) {
    return json_error(status, error);
  }
  Response response;
  response.body = std::move(body);
  return response;
}

Response UiServer::Impl::serve_dumps() {
  JsonWriter out;
  out.begin_object();
  out.key("ok");
  out.boolean(true);
  out.key("dumps");
  out.begin_array();
  for (const DumpFileInfo& dump : recorder.list_dumps(128)) {
    out.begin_object();
    out.key("name");
    out.string(dump.name);
    out.key("size_bytes");
    out.number(dump.size_bytes);
    out.key("valid");
    out.boolean(dump.valid);
    out.key("error");
    out.string(dump.error);
    out.key("flags");
    out.number(static_cast<std::uint64_t>(dump.meta.flags));
    out.key("created_wall_ns");
    out.number(dump.meta.created_wall_ns);
    out.key("record_count");
    out.number(dump.meta.record_count);
    out.key("first_seq");
    out.number(dump.meta.first_seq);
    out.key("last_seq");
    out.number(dump.meta.last_seq);
    out.key("time");
    if (dump.meta.created_wall_ns > 0) {
      out.string(format_wall_ns(dump.meta.created_wall_ns));
    } else {
      out.null();
    }
    out.end_object();
  }
  out.end_array();
  out.end_object();
  if (!out.ok()) {
    return json_error(500, "dump listing too large");
  }
  Response response;
  response.body = out.str();
  return response;
}

Response UiServer::Impl::serve_download(const Request& request) {
  const std::string* name = request.param("name");
  if (name == nullptr || name->empty()) {
    return json_error(400, "missing dump name");
  }
  const auto dumps = recorder.list_dumps(128);
  const DumpFileInfo* found = nullptr;
  for (const auto& dump : dumps) {
    if (dump.name == *name) {
      found = &dump;
      break;
    }
  }
  if (found == nullptr) {
    return json_error(404, "dump not found");
  }
  std::string bytes;
  std::string error;
  if (!read_file_bounded(found->path, kMaxDownloadBytes, bytes, error)) {
    return json_error(500, error);
  }
  Response response;
  response.content_type = "application/octet-stream";
  response.body = std::move(bytes);
  response.extra_headers.emplace_back(
      "Content-Disposition",
      "attachment; filename=\"" + safe_filename(found->name) + "\"");
  return response;
}

Response UiServer::Impl::serve_open(const Request& request) {
  const std::string* name = request.param("name");
  if (name == nullptr || name->empty()) {
    return json_error(400, "missing dump name");
  }
  const auto dumps = recorder.list_dumps(128);
  const DumpFileInfo* found = nullptr;
  for (const auto& dump : dumps) {
    if (dump.name == *name) {
      found = &dump;
      break;
    }
  }
  if (found == nullptr) {
    return json_error(404, "dump not found");
  }
  DumpReadOptions options;
  options.require_footer = false;
  DumpReadResult result = read_dump_file(found->path, options);
  if (!result.ok) {
    return json_error(400, "cannot open dump: " + result.error);
  }
  JsonWriter out;
  out.begin_object();
  out.key("ok");
  out.boolean(true);
  out.key("dump");
  out.begin_object();
  out.key("name");
  out.string(found->name);
  out.key("flags");
  out.number(static_cast<std::uint64_t>(result.meta.flags));
  out.key("created_wall_ns");
  out.number(result.meta.created_wall_ns);
  out.key("record_count");
  out.number(result.meta.record_count);
  out.key("complete");
  out.boolean(result.complete);
  out.key("truncated");
  out.boolean(result.truncated);
  out.key("corrupted");
  out.boolean(result.corrupted);
  out.key("error");
  out.string(result.error);
  out.key("time");
  if (result.meta.created_wall_ns > 0) {
    out.string(format_wall_ns(result.meta.created_wall_ns));
  } else {
    out.null();
  }
  out.end_object();
  std::string analysis_error;
  write_analysis(out, result.records, analysis_error);
  out.end_object();
  if (!out.ok()) {
    return json_error(500, "opened dump is too large to render");
  }
  Response response;
  response.body = out.str();
  return response;
}

Response UiServer::Impl::serve_flush() {
  const DumpResult result = recorder.flush_dump();
  JsonWriter out;
  out.begin_object();
  out.key("ok");
  out.boolean(result.ok);
  out.key("name");
  if (result.path.empty()) {
    out.string("");
  } else {
    out.string(std::filesystem::path(result.path).filename().string());
  }
  out.key("error");
  out.string(result.error);
  out.end_object();
  if (!out.ok()) {
    return json_error(500, "could not encode dump response");
  }
  Response response;
  response.status = result.ok ? 200 : 500;
  response.body = out.str();
  return response;
}

void UiServer::Impl::serve_sse(int fd, const Request& request) {
  if (!authorized(request)) {
    (void)ui::send_response(fd, json_error(401, "invalid or missing token"));
    return;
  }
  const int previous = active_sse.fetch_add(1);
  if (previous >= static_cast<int>(kMaxSseConnections)) {
    active_sse.fetch_sub(1);
    (void)ui::send_response(fd, json_error(429, "too many event streams"));
    return;
  }
  struct Guard {
    std::atomic<int>& counter;
    ~Guard() { counter.fetch_sub(1); }
  } guard{active_sse};

  static constexpr char kHeaders[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream; charset=utf-8\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "X-Accel-Buffering: no\r\n\r\n";
  if (!ui::send_all(fd, kHeaders, sizeof(kHeaders) - 1)) {
    return;
  }
  while (running.load(std::memory_order_acquire)) {
    std::string body;
    std::string error;
    int status = 200;
    if (!build_snapshot(request, 800, body, status, error)) {
      JsonWriter failure;
      failure.begin_object();
      failure.key("ok");
      failure.boolean(false);
      failure.key("error");
      failure.string(error);
      failure.end_object();
      const std::string event =
          "event: error\ndata: " + failure.str() + "\n\n";
      if (!ui::send_all(fd, event.data(), event.size())) {
        return;
      }
    } else {
      const std::string event = "event: snapshot\ndata: " + body + "\n\n";
      if (!ui::send_all(fd, event.data(), event.size())) {
        return;
      }
    }
    for (int tick = 0; tick < 20; ++tick) {
      if (!running.load(std::memory_order_acquire)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const char heartbeat[] = ": keepalive\n\n";
    if (!ui::send_all(fd, heartbeat, sizeof(heartbeat) - 1)) {
      return;
    }
  }
}

Response UiServer::Impl::route(const Request& request) {
  if (request.path == "/api/health") {
    JsonWriter out;
    out.begin_object();
    out.key("ok");
    out.boolean(true);
    out.key("product");
    out.string("blackbox");
    out.key("version");
    out.string(kVersion);
    out.key("crash_handler");
    out.boolean(recorder.crash_handler_active());
    out.key("uptime_ms");
    out.number(started_wall_ns > 0 ? (now_wall_ns() - started_wall_ns) / 1'000'000 : 0);
    out.end_object();
    Response response;
    response.body = out.str();
    return response;
  }
  if (!authorized(request)) {
    return json_error(401, "invalid or missing token");
  }
  if (request.method == "GET" && request.path == "/") {
    return serve_root(request);
  }
  if (request.method == "GET" && request.path == "/api/snapshot") {
    return serve_snapshot(request, kDefaultLimit);
  }
  if (request.method == "GET" && request.path == "/api/stats") {
    JsonWriter out;
    out.begin_object();
    out.key("ok");
    out.boolean(true);
    out.key("stats");
    write_recorder_stats_json(out, recorder.stats());
    out.key("crash_handler");
    out.boolean(recorder.crash_handler_active());
    out.key("crash_error");
    out.string(recorder.crash_error());
    out.key("uptime_ms");
    out.number(started_wall_ns > 0 ? (now_wall_ns() - started_wall_ns) / 1'000'000 : 0);
    out.end_object();
    if (!out.ok()) {
      return json_error(500, "stats response too large");
    }
    Response response;
    response.body = out.str();
    return response;
  }
  if (request.method == "GET" && request.path == "/api/query") {
    return serve_snapshot(request, 500);
  }
  if (request.method == "GET" && request.path == "/api/dumps") {
    return serve_dumps();
  }
  if (request.method == "GET" && request.path == "/api/dump") {
    return serve_download(request);
  }
  if (request.method == "GET" && request.path == "/api/dump/open") {
    return serve_open(request);
  }
  if (request.method == "POST" && request.path == "/api/dump") {
    if (!require_post_token(request)) {
      return json_error(403, "POST requires the X-Blackbox-Token header");
    }
    return serve_flush();
  }
  if (request.method == "POST" && request.path == "/api/demo/trigger") {
    if (!require_post_token(request)) {
      return json_error(403, "POST requires the X-Blackbox-Token header");
    }
    if (!demo_trigger) {
      return json_error(501, "no demo service is attached");
    }
    demo_trigger();
    JsonWriter out;
    out.begin_object();
    out.key("ok");
    out.boolean(true);
    out.end_object();
    Response response;
    response.body = out.str();
    return response;
  }
  if (request.method == "GET" || request.method == "HEAD") {
    return serve_asset(request.path);
  }
  return json_error(404, "not found");
}

UiServer::UiServer(Recorder& recorder, std::function<void()> demo_trigger)
    : impl_(new Impl(recorder, std::move(demo_trigger))) {}

UiServer::~UiServer() { impl_->stop(); }

bool UiServer::start(std::string* error) { return impl_->start(error); }

void UiServer::stop() { impl_->stop(); }

bool UiServer::running() const {
  return impl_->running.load(std::memory_order_acquire);
}

std::uint16_t UiServer::port() const { return impl_->port; }

const std::string& UiServer::token() const { return impl_->token; }

std::string UiServer::url() const {
  return "http://127.0.0.1:" + std::to_string(impl_->port) + "/?token=" +
         impl_->token;
}

bool open_browser(std::string_view url, std::string* error) {
  std::string local_error;
  std::string& err = error != nullptr ? *error : local_error;
  err.clear();
  if (url.empty() || url.size() > 2048 ||
      url.find('\0') != std::string_view::npos) {
    err = "invalid browser URL";
    return false;
  }
  const std::string owned(url);
  const pid_t child = ::fork();
  if (child < 0) {
    err = "cannot fork browser launcher";
    return false;
  }
  if (child == 0) {
#if defined(__APPLE__)
    ::execl("/usr/bin/open", "open", owned.c_str(), static_cast<char*>(nullptr));
#else
    ::execlp("xdg-open", "xdg-open", owned.c_str(),
             static_cast<char*>(nullptr));
#endif
    ::_exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    err = "cannot wait for browser launcher";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    err = "browser launcher failed";
    return false;
  }
  return true;
}

}  // namespace bb
