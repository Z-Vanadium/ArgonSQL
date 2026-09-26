#include "server/tcp_server.h"

#include "executor/session_context.h"
#include "server/protocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utility>

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) {
  // 信号处理函数不能安全地操作 epoll、mutex 或 C++ 容器，因此这里只设置
  // 一个 sig_atomic_t 标记；主循环会在下一次事件循环中执行正常清理。
  g_stop_requested = 1;
}

void InstallStopSignalHandlers() {
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  // 不设置 SA_RESTART，让 epoll_wait/accept 在收到信号后尽快返回。
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

constexpr size_t kReceiveBufferSize = 4096;
constexpr size_t kMaxConnectionInputSize = 1024 * 1024;
constexpr int kMaxEvents = 128;
}  // namespace

TcpServer::TcpServer(std::string host, uint16_t port, size_t worker_count)
    : host_(std::move(host)), port_(port), thread_pool_(worker_count) {}

TcpServer::~TcpServer() {
  Stop();
}

int TcpServer::Run() {
  InstallStopSignalHandlers();

  // 监听 fd 和客户端 fd 都设为非阻塞。Reactor 线程永远不能因为一个
  // 慢客户端而卡在 accept/recv/send 上，否则其它连接无法获得服务。
  listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (listen_fd_ < 0) {
    std::cerr << "Failed to create TCP socket: " << std::strerror(errno) << std::endl;
    return 1;
  }

  int reuse_address = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    std::cerr << "Invalid IPv4 address: " << host_ << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return 1;
  }

  if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
      listen(listen_fd_, 256) < 0) {
    std::cerr << "Failed to bind/listen " << host_ << ":" << port_ << ": " << std::strerror(errno) << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return 1;
  }

  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  completion_event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (epoll_fd_ < 0 || completion_event_fd_ < 0) {
    std::cerr << "Failed to create epoll/eventfd: " << std::strerror(errno) << std::endl;
    Stop();
    return 1;
  }

  epoll_event listen_event{};
  listen_event.events = EPOLLIN;
  listen_event.data.fd = listen_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listen_event);

  epoll_event completion_event{};
  completion_event.events = EPOLLIN;
  completion_event.data.fd = completion_event_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, completion_event_fd_, &completion_event);

  std::cout << "ArgonSQL server listening on " << host_ << ":" << port_ << std::endl;

  // Reactor 线程只做连接管理和 Socket IO。SQL 解析、规划、执行均在
  // thread_pool_ 中完成，因此空闲连接不会占用 SQL worker。
  epoll_event events[kMaxEvents];
  while (!stopping_) {
    const int event_count = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
    if (event_count < 0) {
      if (errno == EINTR) {
        if (g_stop_requested) Stop();
        continue;
      }
      std::cerr << "epoll_wait failed: " << std::strerror(errno) << std::endl;
      Stop();
      break;
    }

    if (g_stop_requested) {
      Stop();
      break;
    }

    for (int i = 0; i < event_count && !stopping_; ++i) {
      const int fd = events[i].data.fd;
      if (fd == listen_fd_) {
        AcceptConnections();
        continue;
      }
      if (fd == completion_event_fd_) {
        DrainCompletedSql();
        continue;
      }

      auto connection_it = connections_.find(fd);
      if (connection_it == connections_.end()) continue;
      const auto connection = connection_it->second;
      const uint32_t event_flags = events[i].events;

      // EPOLLIN 和 EPOLLRDHUP 可能在同一次事件中同时出现：客户端常见的
      // “发送请求后 shutdown(SHUT_WR)”就属于这种情况。必须先读取剩余
      // 请求，再标记 peer_closed，否则最后一条 SQL 会被提前丢弃。
      const bool peer_closed_event = (event_flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
      if ((event_flags & EPOLLIN) != 0) {
        HandleReadable(connection);
      }
      if (connections_.find(fd) == connections_.end()) continue;
      if (peer_closed_event) connection->peer_closed = true;
      if (connections_.find(fd) == connections_.end()) continue;
      if ((event_flags & EPOLLOUT) != 0) HandleWritable(connection);
      if (connections_.find(fd) == connections_.end()) continue;

      if (connection->peer_closed && !connection->sql_in_flight &&
          connection->output_offset == connection->output_buffer.size()) {
        CloseClient(connection);
      } else {
        UpdateConnectionEvents(connection);
      }
    }
  }

  return 0;
}

void TcpServer::AcceptConnections() {
  while (!stopping_) {
    sockaddr_in client_address{};
    socklen_t client_address_size = sizeof(client_address);
    const int client_fd = accept4(listen_fd_, reinterpret_cast<sockaddr *>(&client_address), &client_address_size,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      std::cerr << "Failed to accept client: " << std::strerror(errno) << std::endl;
      return;
    }

    const uint64_t connection_id = next_session_id_.fetch_add(1);
    auto connection = std::make_shared<Connection>();
    connection->fd = client_fd;
    connection->connection_id = connection_id;
    connection->session = std::make_shared<SessionContext>(connection_id);

    epoll_event client_event{};
    client_event.events = EPOLLIN | EPOLLRDHUP;
    client_event.data.fd = client_fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &client_event) < 0) {
      close(client_fd);
      continue;
    }

    connections_.emplace(client_fd, connection);
    connections_by_id_.emplace(connection_id, connection);
    {
      std::lock_guard<std::mutex> lock(clients_latch_);
      active_clients_.insert(client_fd);
    }
  }
}

void TcpServer::HandleReadable(const std::shared_ptr<Connection> &connection) {
  char buffer[kReceiveBufferSize];
  while (true) {
    const ssize_t received = recv(connection->fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      connection->input_buffer.append(buffer, static_cast<size_t>(received));
      if (connection->input_buffer.size() > kMaxConnectionInputSize) {
        connection->peer_closed = true;
        connection->close_after_write = true;
        connection->output_buffer = "ERROR: SQL request is too large.\n";
        connection->output_offset = 0;
        UpdateConnectionEvents(connection);
        return;
      }
      continue;
    }
    if (received == 0) {
      connection->peer_closed = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    connection->peer_closed = true;
    break;
  }

  // 如果上一个 SQL 还在 worker 中执行，只缓存输入，不提交第二个 SQL。
  // 这样既能吸收 TCP 粘包，也能保证同一 Session 的请求严格有序。
  SubmitNextSql(connection);
  if (connection->peer_closed && !connection->sql_in_flight && connection->output_buffer.empty()) {
    CloseClient(connection);
  } else {
    UpdateConnectionEvents(connection);
  }
}

bool TcpServer::ExtractNextSql(const std::shared_ptr<Connection> &connection, std::string *sql) {
  const std::string protocol_prefix = std::string(ArgonSQLProtocol::kVersion);
  if (connection->mode == ProtocolMode::kUnknown) {
    // 第一次 recv 可能只拿到版本字符串的一部分。只要当前内容仍可能
    // 是协议前缀，就等待更多字节，而不是误判成旧协议。
    if (connection->input_buffer.size() < protocol_prefix.size() &&
        protocol_prefix.rfind(connection->input_buffer, 0) == 0) {
      return false;
    }
    connection->mode = connection->input_buffer.rfind(protocol_prefix, 0) == 0
                           ? ProtocolMode::kFramedProtocol
                           : ProtocolMode::kLegacySql;
  }

  if (connection->mode == ProtocolMode::kFramedProtocol) {
    const size_t header_end = connection->input_buffer.find('\n');
    if (header_end == std::string::npos) return false;

    size_t payload_size = 0;
    const std::string header = connection->input_buffer.substr(0, header_end);
    if (!ArgonSQLProtocol::ParseRequestHeader(header, &payload_size)) {
      const std::string payload = "ERROR: invalid ArgonSQL protocol request.\n";
      connection->output_buffer = ArgonSQLProtocol::BuildResponseHeader(
                                      ArgonSQLProtocol::ResponseStatus::kError, payload.size()) +
                                  payload;
      connection->output_offset = 0;
      connection->peer_closed = true;
      connection->close_after_write = true;
      return false;
    }

    const size_t frame_start = header_end + 1;
    if (connection->input_buffer.size() < frame_start + payload_size) return false;
    *sql = connection->input_buffer.substr(frame_start, payload_size);
    connection->input_buffer.erase(0, frame_start + payload_size);
    return true;
  }

  // 旧协议没有长度字段，只能用分号判断一条 SQL 的结束位置。
  const size_t delimiter = connection->input_buffer.find(';');
  if (delimiter == std::string::npos) return false;
  *sql = connection->input_buffer.substr(0, delimiter + 1);
  connection->input_buffer.erase(0, delimiter + 1);
  return true;
}

void TcpServer::SubmitNextSql(const std::shared_ptr<Connection> &connection) {
  // peer_closed 只表示客户端不会再发送新字节；已经进入 input_buffer 的
  // 完整请求仍然必须执行。例如客户端发送完最后一条 SQL 后调用
  // shutdown(SHUT_WR)，Server 仍应返回这条 SQL 的结果。
  if (connection->sql_in_flight) return;

  std::string sql;
  if (!ExtractNextSql(connection, &sql)) return;
  if (!SubmitSql(connection, sql)) CloseClient(connection);
}

bool TcpServer::SubmitSql(const std::shared_ptr<Connection> &connection, const std::string &sql) {
  const uint64_t request_id = connection->next_request_id++;
  const uint64_t connection_id = connection->connection_id;
  const auto session = connection->session;
  connection->in_flight_request_id = request_id;
  connection->sql_in_flight = true;

  // worker 只持有 Session 和 SQL，不持有或操作 Socket。即使客户端在
  // SQL 执行期间断开，worker 仍能安全结束，Reactor 会丢弃过期结果。
  const bool submitted = thread_pool_.Submit([this, connection_id, request_id, session, sql] {
    std::string response;
    bool should_quit = false;
    const bool success = ExecuteSql(sql, session.get(), &response, &should_quit);
    PushCompletedSql({connection_id, request_id, success, should_quit, std::move(response)});
  });
  if (!submitted) connection->sql_in_flight = false;
  return submitted;
}

void TcpServer::PushCompletedSql(CompletedSql result) {
  {
    std::lock_guard<std::mutex> lock(completed_latch_);
    completed_sql_.push_back(std::move(result));
  }

  // eventfd 是 worker 到 Reactor 的轻量唤醒通道。worker 不直接调用
  // epoll_ctl，也不碰连接 map，从而把连接生命周期集中到一个线程管理。
  const uint64_t wakeup = 1;
  if (completion_event_fd_ >= 0) write(completion_event_fd_, &wakeup, sizeof(wakeup));
}

void TcpServer::DrainCompletedSql() {
  uint64_t wakeup = 0;
  while (completion_event_fd_ >= 0 && read(completion_event_fd_, &wakeup, sizeof(wakeup)) > 0) {
  }

  std::deque<CompletedSql> completed;
  {
    std::lock_guard<std::mutex> lock(completed_latch_);
    completed.swap(completed_sql_);
  }

  for (auto &result : completed) {
    auto connection_it = connections_by_id_.find(result.connection_id);
    if (connection_it == connections_by_id_.end()) continue;
    const auto connection = connection_it->second;
    if (!connection->sql_in_flight || connection->in_flight_request_id != result.request_id) continue;

    std::string response = std::move(result.response);
    if (result.success && response.empty()) response = "OK\n";
    if (connection->mode == ProtocolMode::kFramedProtocol) {
      const auto status = result.should_quit
                              ? ArgonSQLProtocol::ResponseStatus::kQuit
                              : result.success ? ArgonSQLProtocol::ResponseStatus::kOk
                                                : ArgonSQLProtocol::ResponseStatus::kError;
      connection->output_buffer.append(ArgonSQLProtocol::BuildResponseHeader(status, response.size()));
    }
    connection->output_buffer.append(response);
    connection->sql_in_flight = false;
    connection->close_after_write = connection->close_after_write || result.should_quit;

    // 只有上一条 SQL 完成后才提交该连接缓存中的下一条请求；不同连接
    // 的任务仍然可以同时存在于线程池中。
    if (!connection->close_after_write) SubmitNextSql(connection);
    UpdateConnectionEvents(connection);
  }
}

void TcpServer::HandleWritable(const std::shared_ptr<Connection> &connection) {
  while (connection->output_offset < connection->output_buffer.size()) {
    const char *data = connection->output_buffer.data() + connection->output_offset;
    const size_t remaining = connection->output_buffer.size() - connection->output_offset;
    const ssize_t sent = send(connection->fd, data, remaining, MSG_NOSIGNAL);
    if (sent > 0) {
      connection->output_offset += static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    CloseClient(connection);
    return;
  }

  connection->output_buffer.clear();
  connection->output_offset = 0;
  if (connection->close_after_write || connection->peer_closed) {
    if (!connection->sql_in_flight) CloseClient(connection);
  } else {
    UpdateConnectionEvents(connection);
  }
}

void TcpServer::UpdateConnectionEvents(const std::shared_ptr<Connection> &connection) {
  if (epoll_fd_ < 0 || connections_.find(connection->fd) == connections_.end()) return;
  epoll_event event{};
  event.events = EPOLLRDHUP;
  if (!connection->peer_closed) event.events |= EPOLLIN;
  if (connection->output_offset < connection->output_buffer.size()) event.events |= EPOLLOUT;
  event.data.fd = connection->fd;
  epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, connection->fd, &event);
}

void TcpServer::Stop() {
  if (stopping_.exchange(true)) return;

  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }

  // 关闭连接会唤醒所有 epoll/Socket 操作；worker 不直接持有 fd，因此
  // 可以安全等待线程池完成尚未结束的 SQL。
  std::vector<std::shared_ptr<Connection>> connections;
  connections.reserve(connections_.size());
  for (const auto &entry : connections_) connections.push_back(entry.second);
  for (const auto &connection : connections) CloseClient(connection);
  thread_pool_.Shutdown();

  if (completion_event_fd_ >= 0) {
    close(completion_event_fd_);
    completion_event_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

void TcpServer::CloseClient(const std::shared_ptr<Connection> &connection) {
  if (connections_.find(connection->fd) == connections_.end()) return;
  if (epoll_fd_ >= 0) epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, connection->fd, nullptr);
  shutdown(connection->fd, SHUT_RDWR);
  close(connection->fd);
  connections_.erase(connection->fd);
  connections_by_id_.erase(connection->connection_id);
  {
    std::lock_guard<std::mutex> lock(clients_latch_);
    active_clients_.erase(connection->fd);
  }
}

bool TcpServer::ExecuteSql(const std::string &sql, SessionContext *session, std::string *response,
                           bool *should_quit) {
  ParserResult parsed = session->GetParser()->Parse(sql);
  if (!parsed.IsSuccess()) {
    *response = std::string("ERROR: ") + parsed.error + "\n";
    return false;
  }

  ExecuteResult result = engine_.Execute(parsed.root, session);
  *response = result.output;
  *should_quit = result.ShouldQuit();
  session->GetParser()->Reset();
  return result.IsSuccess() || result.ShouldQuit();
}
