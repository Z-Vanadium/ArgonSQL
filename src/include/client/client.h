#ifndef ARGONSQL_CLIENT_H
#define ARGONSQL_CLIENT_H

#include <cstdint>
#include <string>

#include "server/protocol.h"

struct ClientResponse {
  // status 是机器可判断的结果，payload 是用户可读的 SQL 输出。两者分离
  // 让上层程序不必解析 ResultWriter 的文本来判断 SQL 是否成功。
  ArgonSQLProtocol::ResponseStatus status{ArgonSQLProtocol::ResponseStatus::kError};
  std::string payload;

  bool IsSuccess() const { return status == ArgonSQLProtocol::ResponseStatus::kOk; }
  bool ShouldQuit() const { return status == ArgonSQLProtocol::ResponseStatus::kQuit; }
};

/**
 * 一个持久 TCP 连接对应一个客户端对象，按请求顺序收发响应。
 *
 * Client 不保存数据库内核状态；当前数据库、事务和锁都由 Server 端
 * 的 SessionContext 管理。这样客户端可以很轻量，也避免多个客户端进程
 * 各自打开同一个数据库文件造成持久化和并发控制冲突。
 */
class ArgonSQLClient {
 public:
  ArgonSQLClient(std::string host, uint16_t port);
  ~ArgonSQLClient();

  ArgonSQLClient(const ArgonSQLClient &) = delete;
  ArgonSQLClient &operator=(const ArgonSQLClient &) = delete;

  /** 建立 TCP 连接；失败时将系统错误转换为可显示文本。 */
  bool Connect(std::string *error);

  /** 发送一条 SQL 并阻塞等待对应的完整响应。 */
  ClientResponse Execute(const std::string &sql);

  /** 主动关闭连接；析构函数也会调用它，因此可重复调用。 */
  void Close();

 private:
  std::string host_;
  uint16_t port_;
  int socket_fd_{-1};
};

#endif  // ARGONSQL_CLIENT_H
