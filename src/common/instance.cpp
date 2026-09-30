//
// Created by njz on 2023/1/15.
//
#include "common/instance.h"

DBStorageEngine::DBStorageEngine(std::string db_name, bool init, uint32_t buffer_pool_size)
    : db_file_name_(std::move(db_name)), init_(init) {
  // Init database file if needed
  db_file_name_ = "./databases/" + db_file_name_;
  if (init_) {
    remove(db_file_name_.c_str());
  }
  // Initialize components
  disk_mgr_ = new DiskManager(db_file_name_);
  bpm_ = new BufferPoolManager(buffer_pool_size, disk_mgr_);

  // Allocate static page for db storage engine
  if (init) {
    page_id_t id;
    if (!bpm_->IsPageFree(CATALOG_META_PAGE_ID)) {
      throw logic_error("Catalog meta page not free.");
    }
    if (!bpm_->IsPageFree(INDEX_ROOTS_PAGE_ID)) {
      throw logic_error("Header page not free.");
    }
    if (bpm_->NewPage(id) == nullptr || id != CATALOG_META_PAGE_ID) {
      throw logic_error("Failed to allocate catalog meta page.");
    }
    if (bpm_->NewPage(id) == nullptr || id != INDEX_ROOTS_PAGE_ID) {
      throw logic_error("Failed to allocate header page.");
    }
    if (bpm_->IsPageFree(CATALOG_META_PAGE_ID) || bpm_->IsPageFree(INDEX_ROOTS_PAGE_ID)) {
      exit(1);
    }
    bpm_->UnpinPage(CATALOG_META_PAGE_ID, false);
    bpm_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
  } else {
    ASSERT(!bpm_->IsPageFree(CATALOG_META_PAGE_ID), "Invalid catalog meta page.");
    ASSERT(!bpm_->IsPageFree(INDEX_ROOTS_PAGE_ID), "Invalid header page.");
  }
  // LockManager、TxnManager 和 LogManager 属于一个数据库实例，而不是
  // 一个客户端连接。这样多个 Session 可以共享同一套锁表和 WAL 文件。
  lock_mgr_ = new LockManager();
  txn_mgr_ = new TxnManager(lock_mgr_);
  log_mgr_ = new LogManager(db_file_name_ + ".wal");
  txn_mgr_->SetLogManager(log_mgr_);
  catalog_mgr_ = new CatalogManager(bpm_, lock_mgr_, log_mgr_, init);
}

DBStorageEngine::~DBStorageEngine() {
  delete catalog_mgr_;
  delete txn_mgr_;
  delete lock_mgr_;
  delete log_mgr_;
  delete bpm_;
  delete disk_mgr_;
}

std::unique_ptr<ExecuteContext> DBStorageEngine::MakeExecuteContext(Txn *txn) {
  return std::make_unique<ExecuteContext>(txn, catalog_mgr_, bpm_, lock_mgr_, log_mgr_, txn_mgr_);
}
