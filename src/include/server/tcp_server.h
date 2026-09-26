#ifndef ARGONSQL_TCP_SERVER_H
#define ARGONSQL_TCP_SERVER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

#include "executor/execute_engine.h"
#include "server/thread_pool.h"

class TcpServer {
 public:
  TcpServer(std::string host, uint16_t port, size_t worker_count = 4);
  ~TcpServer();

  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;

  /** 创建监听 socket，并持续接收客户端连接。 */
  int Run();

  /** 停止接收新连接并关闭监听 socket。 */
  void Stop();

 private:
  /** 处理一个客户端连接，连接关闭前可以连续发送多条 SQL。 */
  void HandleClient(int client_fd, uint64_t session_id);

  /** 从活动连接表中移除并关闭客户端 socket。 */
  void CloseClient(int client_fd);

  /** 确保完整响应都写入 socket，处理 send 的部分写入情况。 */
  bool SendAll(int client_fd, const std::string &message) const;

  /** 解析并执行一条 SQL；不同连接可同时在不同 worker 中执行。 */
  bool ExecuteSql(const std::string &sql, SessionContext *session, std::string *response, bool *should_quit);

  std::string host_;
  uint16_t port_;
  int listen_fd_{-1};
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> next_session_id_{1};

  /** 所有连接共享数据库实例、Catalog 和 Buffer Pool。 */
  ExecuteEngine engine_;

  /** 固定数量的工作线程，避免为每个连接无限创建线程。 */
  ThreadPool thread_pool_;

  /** 记录尚未结束的 socket，停止服务时用于唤醒阻塞中的 recv。 */
  std::mutex clients_latch_;
  std::unordered_set<int> active_clients_;

};

#endif  // ARGONSQL_TCP_SERVER_H
