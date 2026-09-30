#include "recovery/log_manager.h"

#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <utility>
#include <unistd.h>

namespace {
const char *LogTypeName(LogRecType type) {
  switch (type) {
    case LogRecType::kBegin:
      return "BEGIN";
    case LogRecType::kCommit:
      return "COMMIT";
    case LogRecType::kAbort:
      return "ABORT";
    case LogRecType::kInsert:
      return "INSERT";
    case LogRecType::kDelete:
      return "DELETE";
    case LogRecType::kUpdate:
      return "UPDATE";
    default:
      return "INVALID";
  }
}
}  // namespace

LogManager::LogManager(std::string wal_path) : wal_path_(std::move(wal_path)) {
  if (!wal_path_.empty()) fd_ = open(wal_path_.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0644);
}

LogManager::~LogManager() {
  Flush();
  if (fd_ >= 0) close(fd_);
}

bool LogManager::Append(const LogRecPtr &record) {
  if (record == nullptr) return false;
  std::ostringstream line;
  line << record->lsn_ << ' ' << record->txn_id_ << ' ' << LogTypeName(record->type_) << ' ' << record->prev_lsn_
       << ' ' << record->old_key_ << ' ' << record->old_val_ << ' ' << record->new_key_ << ' ' << record->new_val_
       << '\n';
  const std::string data = line.str();

  std::lock_guard<std::mutex> lock(latch_);
  if (fd_ < 0) return true;  // 无 WAL 路径的测试实例使用 no-op 日志。
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t count = write(fd_, data.data() + written, data.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    written += static_cast<size_t>(count);
  }
  return true;
}

bool LogManager::Flush() {
  std::lock_guard<std::mutex> lock(latch_);
  return fd_ < 0 || fsync(fd_) == 0;
}
