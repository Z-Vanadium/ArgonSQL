#ifndef MINISQL_INSTANCE_H
#define MINISQL_INSTANCE_H

#include <memory>
#include <string>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "concurrency/txn_manager.h"
#include "common/config.h"
#include "common/dberr.h"
#include "common/macros.h"
#include "executor/execute_context.h"
#include "storage/disk_manager.h"
#include "recovery/log_manager.h"

class DBStorageEngine {
 public:
  explicit DBStorageEngine(std::string db_name, bool init = true, uint32_t buffer_pool_size = DEFAULT_BUFFER_POOL_SIZE);

  ~DBStorageEngine();

  std::unique_ptr<ExecuteContext> MakeExecuteContext(Txn *txn);

  uint64_t GetBufferPoolHitCount() const { return bpm_ == nullptr ? 0 : bpm_->GetFetchHitCount(); }
  uint64_t GetBufferPoolMissCount() const { return bpm_ == nullptr ? 0 : bpm_->GetFetchMissCount(); }
  uint64_t GetLockWaitCount() const { return lock_mgr_ == nullptr ? 0 : lock_mgr_->GetWaitCount(); }
  uint64_t GetLockWaitNanoseconds() const {
    return lock_mgr_ == nullptr ? 0 : lock_mgr_->GetWaitNanoseconds();
  }

 public:
  DiskManager *disk_mgr_;
  BufferPoolManager *bpm_;
  CatalogManager *catalog_mgr_;
  std::string db_file_name_;
  bool init_;
  LockManager *lock_mgr_{nullptr};
  TxnManager *txn_mgr_{nullptr};
  LogManager *log_mgr_{nullptr};
};

#endif  // MINISQL_INSTANCE_H
