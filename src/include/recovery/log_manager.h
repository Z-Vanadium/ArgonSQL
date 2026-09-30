#ifndef MINISQL_LOG_MANAGER_H
#define MINISQL_LOG_MANAGER_H

#include <mutex>
#include <string>

#include "recovery/log_rec.h"

/**
 * 第一版 WAL 管理器。
 *
 * 当前实现重点保证事务边界日志可以进入数据库自己的 .wal 文件，并在
 * COMMIT 前执行 Flush。它还没有完成完整的页级 Redo/Undo；后续可以在
 * 同一接口上增加批量日志缓冲、后台刷盘和页 LSN。
 */
class LogManager {
 public:
  LogManager() = default;
  explicit LogManager(std::string wal_path);
  ~LogManager();

  LogManager(const LogManager &) = delete;
  LogManager &operator=(const LogManager &) = delete;

  /** 追加一条日志；调用者负责保证事务状态和日志顺序。 */
  bool Append(const LogRecPtr &record);

  /** 将已追加日志刷入文件并调用 fsync，作为提交的 WAL 屏障。 */
  bool Flush();

  const std::string &GetPath() const { return wal_path_; }

 private:
  std::string wal_path_;
  int fd_{-1};
  std::mutex latch_;
};

#endif  // MINISQL_LOG_MANAGER_H
