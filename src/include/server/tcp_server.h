#ifndef ARGONSQL_TCP_SERVER_H
#define ARGONSQL_TCP_SERVER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  enum class ProtocolMode { kUnknown, kLegacySql, kFramedProtocol };

  /**
   * Reactor 线程独占的连接状态。
   *
   * Socket 本身只由 epoll 线程读写；SessionContext 只在该连接的 SQL
   * worker 中使用。一个连接同一时刻最多提交一个 SQL，保证 USE DATABASE
   * 和事务状态的顺序语义。
   */
  struct Connection {
    int fd;
    uint64_t connection_id;
    std::shared_ptr<SessionContext> session;
    ProtocolMode mode{ProtocolMode::kUnknown};
    std::string input_buffer;
    std::string output_buffer;
    size_t output_offset{0};
    uint64_t next_request_id{1};
    uint64_t in_flight_request_id{0};
    bool sql_in_flight{false};
    bool peer_closed{false};
    bool close_after_write{false};
  };

  /** SQL worker 完成后投递给 Reactor 的结果，不携带可直接操作的 fd。 */
  struct CompletedSql {
    uint64_t connection_id;
    uint64_t request_id;
    bool success;
    bool should_quit;
    std::string response;
  };

  /** accept 新连接并设置为非阻塞。 */
  void AcceptConnections();

  /** 读取一个连接当前可获得的全部字节，并尝试提交 SQL。 */
  void HandleReadable(const std::shared_ptr<Connection> &connection);

  /** 将输出缓冲区尽可能写入 Socket。 */
  void HandleWritable(const std::shared_ptr<Connection> &connection);

  /** 从输入缓冲区取出一条完整 SQL；不完整时返回空。 */
  bool ExtractNextSql(const std::shared_ptr<Connection> &connection, std::string *sql);

  /** 一个连接完成上一条 SQL 后，按顺序提交下一条 SQL。 */
  void SubmitNextSql(const std::shared_ptr<Connection> &connection);

  /** 将 SQL 任务放入线程池；worker 不直接操作 Socket。 */
  bool SubmitSql(const std::shared_ptr<Connection> &connection, const std::string &sql);

  /** 处理 worker 完成队列并唤醒对应连接的写事件。 */
  void DrainCompletedSql();

  /** 修改某个连接在 epoll 中关注的事件。 */
  void UpdateConnectionEvents(const std::shared_ptr<Connection> &connection);

  /** 从 epoll 和连接表移除并关闭客户端 socket。 */
  void CloseClient(const std::shared_ptr<Connection> &connection);

  /** 向完成队列写入一个结果，并唤醒 epoll 线程。 */
  void PushCompletedSql(CompletedSql result);

  /** 解析并执行一条 SQL；不同连接可同时在不同 worker 中执行。 */
  bool ExecuteSql(const std::string &sql, SessionContext *session, std::string *response, bool *should_quit);

  std::string host_;
  uint16_t port_;
  int listen_fd_{-1};
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> next_session_id_{1};

  int epoll_fd_{-1};
  int completion_event_fd_{-1};

  /** 所有连接共享数据库实例、Catalog 和 Buffer Pool。 */
  ExecuteEngine engine_;

  /** 固定数量的工作线程，避免为每个连接无限创建线程。 */
  ThreadPool thread_pool_;

  /** epoll 线程拥有的连接表；key 是 fd，value 保存协议和 Session 状态。 */
  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  /** 以稳定的 connection_id 查找连接，避免 fd 关闭后被系统复用造成串响应。 */
  std::unordered_map<uint64_t, std::shared_ptr<Connection>> connections_by_id_;

  /** 记录尚未结束的 socket，停止服务时用于关闭全部连接。 */
  std::mutex clients_latch_;
  std::unordered_set<int> active_clients_;

  /** worker 到 Reactor 的完成队列；只有 Reactor 线程修改连接 Socket。 */
  std::mutex completed_latch_;
  std::deque<CompletedSql> completed_sql_;

};

#endif  // ARGONSQL_TCP_SERVER_H
