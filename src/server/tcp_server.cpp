#include "server/tcp_server.h"

#include "executor/session_context.h"

#include <arpa/inet.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) {
  // 信号处理函数只设置一个 sig_atomic_t 标志；socket 关闭和对象析构
  // 留给正常的 Server 控制流执行，避免在信号上下文中操作 C++ 对象。
  g_stop_requested = 1;
}

void InstallStopSignalHandlers() {
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  // 不设置 SA_RESTART，让 accept 在收到 SIGINT/SIGTERM 后返回 EINTR，
  // 主循环才能检查 g_stop_requested 并进入正常析构流程。
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

// recv 每次读取的临时缓冲区大小。SQL 请求可能跨越多次 recv，因此不能
// 假设一次 recv 就对应一条完整 SQL。
constexpr size_t kReceiveBufferSize = 4096;

// 防止客户端持续发送数据耗尽服务端内存。超过限制的请求会被拒绝。
constexpr size_t kMaxRequestSize = 1024 * 1024;
}  // namespace

TcpServer::TcpServer(std::string host, uint16_t port, size_t worker_count)
    : host_(std::move(host)), port_(port), thread_pool_(worker_count) {}

TcpServer::~TcpServer() {
  Stop();
}

int TcpServer::Run() {
  // 让 SIGINT/SIGTERM 走优雅退出路径，使 ExecuteEngine 析构并刷盘。
  InstallStopSignalHandlers();

  // AF_INET 表示当前第一版只支持 IPv4；后续可以通过 getaddrinfo 扩展
  // 到 IPv6 或域名绑定。
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    std::cerr << "Failed to create TCP socket: " << std::strerror(errno) << std::endl;
    return 1;
  }

  int reuse_address = 1;
  // 允许服务重启时快速复用端口，避免上一次连接留下 TIME_WAIT 导致 bind
  // 暂时失败。
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

  if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    std::cerr << "Failed to bind " << host_ << ":" << port_ << ": " << std::strerror(errno) << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return 1;
  }

  if (listen(listen_fd_, 64) < 0) {
    std::cerr << "Failed to listen: " << std::strerror(errno) << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return 1;
  }

  std::cout << "ArgonSQL server listening on " << host_ << ":" << port_ << std::endl;
  while (!stopping_) {
    sockaddr_in client_address{};
    socklen_t client_address_size = sizeof(client_address);
    int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_address), &client_address_size);
    if (client_fd < 0) {
      if (g_stop_requested) {
        Stop();
        break;
      }
      if (stopping_ || errno == EINTR) {
        continue;
      }
      std::cerr << "Failed to accept client: " << std::strerror(errno) << std::endl;
      continue;
    }

    const uint64_t session_id = next_session_id_.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(clients_latch_);
      active_clients_.insert(client_fd);
    }

    // accept 线程不直接处理客户端，而是把任务交给固定数量的 worker。
    // 这样连接数量增长时不会无限创建线程。
    const bool submitted = thread_pool_.Submit([this, client_fd, session_id] {
      HandleClient(client_fd, session_id);
    });
    if (!submitted) {
      CloseClient(client_fd);
    }
  }

  return 0;
}

void TcpServer::Stop() {
  stopping_ = true;
  if (listen_fd_ >= 0) {
    // shutdown 会唤醒阻塞中的 accept；随后 close 释放文件描述符。
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }

  // recv 可能阻塞在活动连接上。先 shutdown 唤醒这些 worker，再等待线程池
  // 中的任务结束，最后由各个 HandleClient 统一释放 fd。
  std::vector<int> clients;
  {
    std::lock_guard<std::mutex> lock(clients_latch_);
    clients.assign(active_clients_.begin(), active_clients_.end());
  }
  for (int client_fd : clients) {
    shutdown(client_fd, SHUT_RDWR);
  }
  thread_pool_.Shutdown();
}

void TcpServer::CloseClient(int client_fd) {
  {
    std::lock_guard<std::mutex> lock(clients_latch_);
    active_clients_.erase(client_fd);
  }
  shutdown(client_fd, SHUT_RDWR);
  close(client_fd);
}

void TcpServer::HandleClient(int client_fd, uint64_t session_id) {
  // session 的生命周期与 TCP 连接一致。客户端断开后，其当前数据库和
  // 未来的事务状态都会随会话销毁。
  SessionContext session(session_id);
  std::string pending;
  char buffer[kReceiveBufferSize];

  while (!stopping_) {
    const ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received == 0) {
      break;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    // TCP 是字节流，不保留消息边界。因此先追加到 pending，再按分号
    // 拆分，能够同时处理半条 SQL 和一次收到多条 SQL 的情况。
    pending.append(buffer, static_cast<size_t>(received));
    if (pending.size() > kMaxRequestSize) {
      SendAll(client_fd, "ERROR: SQL request is too large.\n");
      break;
    }

    size_t delimiter = pending.find(';');
    while (delimiter != std::string::npos) {
      std::string sql = pending.substr(0, delimiter + 1);
      pending.erase(0, delimiter + 1);

      std::string response;
      bool should_quit = false;
      // 空输出的成功命令（例如某些 DDL）仍要给客户端一个明确响应，
      // 否则客户端无法判断服务端是否已经处理完该请求。
      if (ExecuteSql(sql, &session, &response, &should_quit) && response.empty()) {
        response = "OK\n";
      }
      if (!SendAll(client_fd, response)) {
        CloseClient(client_fd);
        return;
      }
      if (should_quit) {
        // QUIT 的响应先发送，再关闭当前客户端连接。
        CloseClient(client_fd);
        return;
      }
      delimiter = pending.find(';');
    }
  }

  CloseClient(client_fd);
}

bool TcpServer::SendAll(int client_fd, const std::string &message) const {
  size_t sent = 0;
  while (sent < message.size()) {
    // send 可能只写入部分数据，必须循环直到完整响应发送完毕。
    // MSG_NOSIGNAL 防止对端提前断开时进程收到 SIGPIPE。
    const ssize_t count = send(client_fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<size_t>(count);
  }
  return true;
}

bool TcpServer::ExecuteSql(const std::string &sql, SessionContext *session, std::string *response,
                           bool *should_quit) {
  // ParserContext 属于当前连接，且生成 parser 的状态已经是线程局部的；
  // 因此解析阶段不再占用 Server 的全局执行锁，不同连接可以并行解析。
  ParserResult parsed = session->GetParser()->Parse(sql);
  if (!parsed.IsSuccess()) {
    *response = std::string("ERROR: ") + parsed.error + "\n";
    return false;
  }

  // ExecuteEngine 为本次请求绑定独立输出流和 SessionContext；只读请求
  // 使用共享锁，写请求使用排他锁，因此不同连接可以真正并行执行 SQL。
  ExecuteResult result = engine_.Execute(parsed.root, session);
  *response = result.output;
  *should_quit = result.ShouldQuit();
  // ExecuteEngine 已经消费完语法树，现在释放本次请求的节点和 lexer buffer。
  session->GetParser()->Reset();
  return result.IsSuccess() || result.ShouldQuit();
}
