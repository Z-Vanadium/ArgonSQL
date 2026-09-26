#include "server/protocol.h"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <sys/socket.h>

#include <limits>

namespace ArgonSQLProtocol {
namespace {

// 协议头只包含版本、消息类型、状态和十进制长度，128 字节足以覆盖
// 当前格式。限制头部长度可以防止恶意客户端发送永不结束的超长头部。
constexpr size_t kMaxHeaderSize = 128;

// 单帧限制同时保护 Server 的 pending 缓冲区和 Client 的结果缓冲区。
// 当前协议传输文本 SQL 和文本结果，1 MiB 足以覆盖普通请求，又能避免
// 一个连接无界地占用内存；以后可以在 ARGONSQL/2 中增加分片传输。
constexpr size_t kMaxFrameSize = 1024 * 1024;

bool SendAll(int fd, const char *data, size_t size) {
  // TCP 是字节流，send() 返回成功只代表部分字节已经进入内核发送缓冲区。
  // 如果这里不循环，长 SQL 或长查询结果会被静默截断。
  size_t sent = 0;
  while (sent < size) {
    const ssize_t count = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    sent += static_cast<size_t>(count);
  }
  return true;
}

bool ReadExact(int fd, char *data, size_t size) {
  // recv() 同样可能只返回一部分数据；按协议长度读取时必须补齐到 size。
  // 对端返回 0 表示有序关闭，不能把它当作“暂时没有数据”。
  size_t received = 0;
  while (received < size) {
    const ssize_t count = recv(fd, data + received, size - received, 0);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    received += static_cast<size_t>(count);
  }
  return true;
}

bool ReadLine(int fd, std::string *line) {
  // 头部采用单行文本便于调试和版本识别，但 payload 不采用换行分隔。
  // 逐字节读取牺牲少量头部性能，换来实现简单且不会提前吞掉 payload；
  // 每个头部最多 128 字节，普通客户端连接的代价可以忽略。
  line->clear();
  while (line->size() < kMaxHeaderSize) {
    char ch = 0;
    if (!ReadExact(fd, &ch, 1)) return false;
    if (ch == '\n') return true;
    line->push_back(ch);
  }
  return false;
}

bool ParseLength(const char *begin, const char *end, size_t *length) {
  // 使用 from_chars 避免 atoi 对非法字符“部分成功”的宽松行为。例如
  // "10abc" 必须被拒绝，否则客户端和服务器会对下一帧边界产生分歧。
  unsigned long long parsed = 0;
  auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed > kMaxFrameSize) return false;
  *length = static_cast<size_t>(parsed);
  return true;
}

const char *StatusText(ResponseStatus status) {
  switch (status) {
    case ResponseStatus::kOk:
      return "OK";
    case ResponseStatus::kError:
      return "ERROR";
    case ResponseStatus::kQuit:
      return "QUIT";
  }
  return "ERROR";
}

}  // namespace

bool ParseRequestHeader(const std::string &header, size_t *payload_size) {
  // 请求头必须完全匹配“版本 + REQUEST + 十进制长度”，不接受额外字段。
  // 这是有意的保守策略：协议扩展应通过版本升级，而不是让旧实现误解新字段。
  const std::string prefix = std::string(kVersion) + " REQUEST ";
  if (header.rfind(prefix, 0) != 0) return false;
  return ParseLength(header.data() + prefix.size(), header.data() + header.size(), payload_size);
}

std::string BuildResponseHeader(ResponseStatus status, size_t payload_size) {
  // 响应头先写状态，再写 payload 长度；客户端可以在尚未读取结果正文时
  // 先知道这是成功、错误还是退出响应，并据此决定连接生命周期。
  return std::string(kVersion) + " RESPONSE " + StatusText(status) + " " + std::to_string(payload_size) + "\n";
}

bool SendResponse(int fd, ResponseStatus status, const std::string &payload) {
  // 头和正文分两次 send，但接收端不能假设对应两次 recv；长度前缀保证
  // 无论内核如何合并/拆分 TCP 段，接收端都能恢复出一条完整响应。
  if (payload.size() > kMaxFrameSize) return false;
  const std::string header = BuildResponseHeader(status, payload.size());
  return SendAll(fd, header.data(), header.size()) && SendAll(fd, payload.data(), payload.size());
}

bool SendRequest(int fd, const std::string &sql) {
  // 客户端一次只发送一条 SQL。Server 在同一连接内按帧顺序执行，因而
  // Session 的 USE DATABASE 和事务状态不会出现请求之间的竞态。
  if (sql.size() > kMaxFrameSize) return false;
  const std::string header = std::string(kVersion) + " REQUEST " + std::to_string(sql.size()) + "\n";
  return SendAll(fd, header.data(), header.size()) && SendAll(fd, sql.data(), sql.size());
}

bool ReceiveResponse(int fd, ResponseStatus *status, std::string *payload) {
  // 一个 ArgonSQLClient 的调用约定是：SendRequest 后立即 ReceiveResponse。
  // 这是一种“单连接单飞请求”模型，保证同一连接的响应顺序；不同连接
  // 仍然可以由 Server 的不同 worker 并行执行。
  std::string header;
  if (!ReadLine(fd, &header)) return false;

  const std::string prefix = std::string(kVersion) + " RESPONSE ";
  if (header.rfind(prefix, 0) != 0) return false;
  const size_t status_begin = prefix.size();
  const size_t status_end = header.find(' ', status_begin);
  if (status_end == std::string::npos) return false;

  if (header.compare(status_begin, status_end - status_begin, "OK") == 0) {
    *status = ResponseStatus::kOk;
  } else if (header.compare(status_begin, status_end - status_begin, "ERROR") == 0) {
    *status = ResponseStatus::kError;
  } else if (header.compare(status_begin, status_end - status_begin, "QUIT") == 0) {
    *status = ResponseStatus::kQuit;
  } else {
    return false;
  }

  size_t payload_size = 0;
  if (!ParseLength(header.data() + status_end + 1, header.data() + header.size(), &payload_size)) return false;
  payload->assign(payload_size, '\0');
  return payload_size == 0 || ReadExact(fd, payload->data(), payload_size);
}

}  // namespace ArgonSQLProtocol
