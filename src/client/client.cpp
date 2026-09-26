#include "client/client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utility>

ArgonSQLClient::ArgonSQLClient(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

ArgonSQLClient::~ArgonSQLClient() { Close(); }

bool ArgonSQLClient::Connect(std::string *error) {
  // 重连前先关闭旧 fd，保证一次 Client 对象最多持有一个连接，避免
  // 发生 fd 泄漏或把响应误读到旧连接上。
  Close();
  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    *error = std::strerror(errno);
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    *error = "invalid IPv4 address: " + host_;
    Close();
    return false;
  }
  if (connect(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    *error = std::strerror(errno);
    Close();
    return false;
  }
  return true;
}

ClientResponse ArgonSQLClient::Execute(const std::string &sql) {
  ClientResponse response;
  // 协议是请求—响应模型：必须先发送完整请求，再读取完整响应。
  // 不允许同一 Client 对象并发调用 Execute，否则两个线程会交叉写入
  // 请求，且无法知道哪个响应属于哪个调用方。需要并行时应创建多个
  // ArgonSQLClient 实例，分别对应多个 TCP 连接。
  if (socket_fd_ < 0 || !ArgonSQLProtocol::SendRequest(socket_fd_, sql) ||
      !ArgonSQLProtocol::ReceiveResponse(socket_fd_, &response.status, &response.payload)) {
    response.status = ArgonSQLProtocol::ResponseStatus::kError;
    response.payload = "ERROR: connection closed or invalid protocol response.\n";
    Close();
  }
  if (response.ShouldQuit()) Close();
  return response;
}

void ArgonSQLClient::Close() {
  // shutdown 先通知对端和本地阻塞系统调用，再 close 释放 fd。即使连接
  // 已经被 Server 关闭，shutdown/close 的失败也不影响客户端释放资源。
  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RDWR);
    close(socket_fd_);
    socket_fd_ = -1;
  }
}
