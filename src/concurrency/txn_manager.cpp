#include "concurrency/txn_manager.h"

#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"

TxnManager::TxnManager(LockManager *lock_mgr) : lock_mgr_(lock_mgr) { lock_mgr_->SetTxnMgr(this); }

TxnManager::~TxnManager() {
  std::unique_lock<std::shared_mutex> lock(rw_latch_);
  // TxnManager 只登记事务，不拥有 Txn 的生命周期。调用方可能传入
  // 栈对象，也可能在 Commit/Abort 后自行 delete 返回的事务指针。
  // 无条件释放会造成 double free；后续由 SessionContext 负责释放
  // 自己创建的事务对象。
  txn_map_.clear();
}

Txn *TxnManager::Begin(Txn *txn, IsolationLevel isolationLevel) {
  if (nullptr == txn) {
    txn = new Txn(next_txn_id_++, isolationLevel);
  }
  std::unique_lock<std::shared_mutex> lock(rw_latch_);
  txn_map_[txn->GetTxnId()] = txn;
  if (log_mgr_ != nullptr) {
    std::lock_guard<std::mutex> log_lock(log_latch_);
    log_mgr_->Append(CreateBeginLog(txn->GetTxnId()));
  }
  return txn;
}

void TxnManager::Commit(Txn *txn) {
  if (txn == nullptr) return;
  // COMMIT 日志必须先落盘，再将事务标记为 committed 并释放锁，形成
  // “客户端收到成功前 WAL 已持久化”的提交屏障。
  if (log_mgr_ != nullptr) {
    std::lock_guard<std::mutex> log_lock(log_latch_);
    log_mgr_->Append(CreateCommitLog(txn->GetTxnId()));
    log_mgr_->Flush();
  }
  // change state
  txn->SetState(TxnState::kCommitted);
  // release all locks
  ReleaseLocks(txn);
  {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    auto it = txn_map_.find(txn->GetTxnId());
    if (it != txn_map_.end() && it->second == txn) txn_map_.erase(it);
  }
}

void TxnManager::Abort(Txn *txn) {
  if (txn == nullptr) return;
  if (log_mgr_ != nullptr) {
    std::lock_guard<std::mutex> log_lock(log_latch_);
    log_mgr_->Append(CreateAbortLog(txn->GetTxnId()));
    log_mgr_->Flush();
  }
  // change state
  txn->SetState(TxnState::kAborted);
  // release all locks
  ReleaseLocks(txn);
  {
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    auto it = txn_map_.find(txn->GetTxnId());
    if (it != txn_map_.end() && it->second == txn) txn_map_.erase(it);
  }
}

Txn *TxnManager::GetTransaction(txn_id_t txn_id) {
  std::shared_lock<std::shared_mutex> lock(rw_latch_);
  auto iter = txn_map_.find(txn_id);
  if (iter != txn_map_.end()) {
    return iter->second;
  }
  return nullptr;
}

void TxnManager::ReleaseLocks(Txn *txn) {
  std::unordered_set<RowId> lock_set;
  for (auto o : txn->GetExclusiveLockSet()) {
    lock_set.emplace(o);
  }
  for (auto o : txn->GetSharedLockSet()) {
    lock_set.emplace(o);
  }
  for (auto rid : lock_set) {
    lock_mgr_->Unlock(txn, rid);
  }
}
