#include "concurrency/lock_manager.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

#include "common/rowid.h"
#include "concurrency/txn.h"
#include "concurrency/txn_manager.h"

void LockManager::SetTxnMgr(TxnManager *txn_mgr) { txn_mgr_ = txn_mgr; }

/**
 * 共享锁申请流程：先检查事务状态，再把请求放入 RID 对应的等待队列。
 * 只有在没有排他持有者、没有升级请求且前面没有排他等待者时，才能授予共享锁。
 * 这样可以避免排他请求长期饥饿。
 */
bool LockManager::LockShared(Txn *txn, const RowId &rid) {
    if(!txn)    return false;

    // ReadUncommitted 不允许共享锁
    if(txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockSharedOnReadUncommitted);
    }

    LockPrepare(txn, rid);

    if(txn->GetState() == TxnState::kAborted || txn->GetState() == TxnState::kCommitted){
        return false;
    }

    std::unique_lock<std::mutex> lock(latch_);

    if(txn->GetExclusiveLockSet().count(rid) != 0 || txn->GetSharedLockSet().count(rid) != 0){
        return true;
    }

    auto& req_q = lock_table_[rid];

    req_q.EmplaceLockRequest(txn->GetTxnId(), LockMode::kShared);
    auto req_it = req_q.GetLockRequestIter(txn->GetTxnId());

    auto has_waiting_writer = [&]() {
        for(const auto& req : req_q.req_list_){
            if(req.lock_mode_ == LockMode::kExclusive && req.granted_ == LockMode::kNone){
                return true;
                break;
            }
        }
        return false;
    };

    try {
        while(req_q.is_writing_ || req_q.is_upgrading_ || has_waiting_writer()){
            CheckAbort(txn, req_q);
            if(txn->GetState() == TxnState::kAborted){
                throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
            }
            const auto wait_begin = std::chrono::steady_clock::now();
            req_q.cv_.wait(lock);
            wait_count_.fetch_add(1, std::memory_order_relaxed);
            wait_nanoseconds_.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_begin).count()),
                std::memory_order_relaxed);
        }
    } catch (...) {
        // CheckAbort 可能在请求仍位于队列中时抛出异常。
        // 必须先移除该等待请求，否则后续事务会把已经中止的请求误认为仍然有效的写请求。
        if (req_q.req_list_iter_map_.find(txn->GetTxnId()) != req_q.req_list_iter_map_.end()) {
            req_q.EraseLockRequest(txn->GetTxnId());
            req_q.cv_.notify_all();
        }
        throw;
    }

    if(txn->GetState() == TxnState::kAborted){
        req_q.EraseLockRequest(txn->GetTxnId());
        req_q.cv_.notify_all();
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
    }

    req_it = req_q.GetLockRequestIter(txn->GetTxnId());
    req_it->granted_ = LockMode::kShared;
    req_q.sharing_cnt_++;
    txn->GetSharedLockSet().insert(rid);
    req_q.cv_.notify_all();

    return true;
}

/**
 * 排他锁申请流程：排他锁必须等待当前 RID 上所有共享锁、排他锁以及更早的等待请求完成。
 * 通过统一的条件变量等待，可以在持锁事务释放锁后及时唤醒竞争者。
 */
bool LockManager::LockExclusive(Txn *txn, const RowId &rid) {
    if(!txn) return false;

    LockPrepare(txn, rid);

    if(txn->GetState() == TxnState::kAborted || txn->GetState() == TxnState::kCommitted){
        return false;
    }

    if(txn->GetExclusiveLockSet().count(rid) != 0){
        return true;
    }

    if(txn->GetSharedLockSet().count(rid) != 0){
        return false;
    }

    std::unique_lock<std::mutex> lock(latch_);

    auto& req_q = lock_table_[rid];

    req_q.EmplaceLockRequest(txn->GetTxnId(), LockMode::kExclusive);
    auto req_it = req_q.GetLockRequestIter(txn->GetTxnId());

    auto is_blocked = [&]() {
        if(req_q.is_writing_ || req_q.sharing_cnt_ > 0) return true;

        for (const auto &request : req_q.req_list_) {
            if (request.txn_id_ != txn->GetTxnId() &&
                request.granted_ == LockMode::kNone) {
                return true;
            }
        }
        return false;
    };

    try {
        while(is_blocked()){
            CheckAbort(txn, req_q);
            const auto wait_begin = std::chrono::steady_clock::now();
            req_q.cv_.wait(lock);
            wait_count_.fetch_add(1, std::memory_order_relaxed);
            wait_nanoseconds_.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_begin).count()),
                std::memory_order_relaxed);
        }
    } catch (...) {
        // 死锁牺牲者不能把未授予的排他锁请求遗留在队列中。
        if (req_q.req_list_iter_map_.find(txn->GetTxnId()) != req_q.req_list_iter_map_.end()) {
            req_q.EraseLockRequest(txn->GetTxnId());
            req_q.cv_.notify_all();
        }
        throw;
    }


    // 等待期间可能被死锁检测线程中止
    if (txn->GetState() == TxnState::kAborted) {
        req_q.EraseLockRequest(txn->GetTxnId());
        req_q.cv_.notify_all();

        throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
    }

    // 授予排他锁
    req_it = req_q.GetLockRequestIter(txn->GetTxnId());
    req_it->granted_ = LockMode::kExclusive;
    req_q.is_writing_ = true;

    // 记录事务持有的排他锁
    txn->GetExclusiveLockSet().insert(rid);

    // 唤醒其他等待线程
    req_q.cv_.notify_all();

    return true;
}

/**
 * 锁升级流程：事务必须先持有共享锁，再将自己的共享请求转换为排他请求。
 * 同一个 RID 同时只允许一个事务升级，否则两个共享持有者都等待对方释放共享锁，
 * 会形成无法自动解决的升级冲突。
 */
bool LockManager::LockUpgrade(Txn *txn, const RowId &rid) {
    if(!txn)    return false;
    LockPrepare(txn, rid);


  if (txn->GetState() == TxnState::kAborted ||
      txn->GetState() == TxnState::kCommitted) {
    return false;
  }

  // 已经是排他锁，不需要重复升级。
  if (txn->GetExclusiveLockSet().count(rid) != 0) {
    return true;
  }

  // 升级的前提是当前事务确实持有共享锁。
  if (txn->GetSharedLockSet().count(rid) == 0) {
    return false;
  }

  std::unique_lock<std::mutex> lock(latch_);

  auto table_iter = lock_table_.find(rid);
  if (table_iter == lock_table_.end()) {
    return false;
  }

  auto &req_queue = table_iter->second;

  // 同一个 RID 上已经有其他事务在升级，直接中止当前事务。
  if (req_queue.is_upgrading_) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kUpgradeConflict);
  }

  auto request_it = req_queue.GetLockRequestIter(txn->GetTxnId());

  // 先设置升级标志，再把请求模式改为排他锁。
  // 此时 granted_ 仍为共享锁，表示事务还没有真正获得 X 锁。
  req_queue.is_upgrading_ = true;
  request_it->lock_mode_ = LockMode::kExclusive;

    auto cleanup_upgrade = [&]() {
    req_queue.is_upgrading_ = false;

    // 当前事务原本持有一个共享锁。重新查找请求，避免外部 Abort/Unlock
    // 已经删除该请求后继续解引用失效的 list 迭代器。
    auto current = req_queue.req_list_iter_map_.find(txn->GetTxnId());
    if (current != req_queue.req_list_iter_map_.end()) {
      if (current->second->granted_ == LockMode::kShared && req_queue.sharing_cnt_ > 0) {
        req_queue.sharing_cnt_--;
      }
      req_queue.EraseLockRequest(txn->GetTxnId());
    }
    txn->GetSharedLockSet().erase(rid);

    req_queue.cv_.notify_all();
  };

  try {
    // sharing_cnt_ == 1 表示只剩当前事务自己的共享锁，可以完成升级。
    while (req_queue.is_writing_ || req_queue.sharing_cnt_ > 1) {
      CheckAbort(txn, req_queue);
      const auto wait_begin = std::chrono::steady_clock::now();
      req_queue.cv_.wait(lock);
      wait_count_.fetch_add(1, std::memory_order_relaxed);
      wait_nanoseconds_.fetch_add(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_begin).count()),
          std::memory_order_relaxed);
    }
  } catch (...) {
    cleanup_upgrade();
    throw;
  }

  // 等待期间可能被后台死锁检测线程中止。
  if (txn->GetState() == TxnState::kAborted) {
    cleanup_upgrade();

    throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
  }

  // 当前事务正式获得排他锁，更新请求状态和事务持锁集合。
  request_it = req_queue.GetLockRequestIter(txn->GetTxnId());
  request_it->granted_ = LockMode::kExclusive;

  req_queue.sharing_cnt_--;
  req_queue.is_writing_ = true;
  req_queue.is_upgrading_ = false;

  // 共享锁升级成功后，必须从共享集合移动到排他集合。
  txn->GetSharedLockSet().erase(rid);
  txn->GetExclusiveLockSet().insert(rid);

  req_queue.cv_.notify_all();

  return true;
}

/**
 * 释放事务持有的锁，并维护事务的锁阶段、锁计数和等待线程通知。
 * 释放第一把锁后事务进入 SHRINKING 阶段；严格两阶段锁协议不允许其再次加锁。
 */
bool LockManager::Unlock(Txn *txn, const RowId &rid) {
    if (txn == nullptr) {
        return false;
    }

    std::unique_lock<std::mutex> lock(latch_);
    auto table_iter = lock_table_.find(rid);
    if (table_iter == lock_table_.end()) {
        return false;
    }

    auto &req_queue = table_iter->second;
    auto request_iter = req_queue.req_list_iter_map_.find(txn->GetTxnId());
    if (request_iter == req_queue.req_list_iter_map_.end()) {
        return false;
    }

    auto request = request_iter->second;
    if (request->granted_ == LockMode::kNone) {
        // 事务中止时可能仍有请求排在等待队列中，需要允许清理该请求。
        if (txn->GetState() != TxnState::kAborted) {
            return false;
        }
        req_queue.EraseLockRequest(txn->GetTxnId());
        // 只有中止的 S -> X 请求拥有升级标志；普通排他等待者不能清除其他事务的升级标志。
        if (request->lock_mode_ == LockMode::kExclusive &&
            txn->GetSharedLockSet().count(rid) != 0) {
            req_queue.is_upgrading_ = false;
        }
        req_queue.cv_.notify_all();
        return true;
    }

    // 严格两阶段锁协议不允许普通事务重复解锁，或在进入 SHRINKING 阶段后继续释放锁。
    // COMMITTED 和 ABORTED 是清理例外，因为 TxnManager 会调用 Unlock 释放它们的剩余锁。
    if (txn->GetState() == TxnState::kShrinking &&
        txn->GetIsolationLevel() == IsolationLevel::kRepeatedRead) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kUnlockOnShrinking);
    }

    if (txn->GetState() == TxnState::kGrowing) {
        txn->SetState(TxnState::kShrinking);
    }

    if (request->granted_ == LockMode::kShared) {
        if (req_queue.sharing_cnt_ > 0) {
            req_queue.sharing_cnt_--;
        }
        txn->GetSharedLockSet().erase(rid);
        // S -> X 升级期间 request mode 已经是 X，但 granted mode 仍是 S；
        // 此时释放该请求必须清除升级标志。
        if (request->lock_mode_ == LockMode::kExclusive) {
            req_queue.is_upgrading_ = false;
        }
    } else if (request->granted_ == LockMode::kExclusive) {
        req_queue.is_writing_ = false;
        req_queue.is_upgrading_ = false;
        txn->GetExclusiveLockSet().erase(rid);
    } else {
        return false;
    }

    req_queue.EraseLockRequest(txn->GetTxnId());
    req_queue.cv_.notify_all();
    return true;
}

/**
 * 执行加锁前的事务状态检查。
 * 在 SHRINKING 阶段加锁违反两阶段锁协议，因此将事务标记为中止并抛出异常。
 */
void LockManager::LockPrepare(Txn *txn, const RowId & /*rid*/) {
    if (txn == nullptr) {
        return;
    }

    // 严格两阶段锁协议中，第一次释放锁会开始 SHRINKING 阶段；之后不能再申请新锁。
    if (txn->GetState() == TxnState::kShrinking) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
    }
}

/**
 * 根据当前等待队列更新事务的等待边，并检查事务是否已经被死锁检测线程中止。
 * 等待图中的边 t1 -> t2 表示 t1 正在等待 t2 持有的冲突锁。
 */
void LockManager::CheckAbort(Txn *txn, LockManager::LockRequestQueue &req_queue) {
    if (txn == nullptr) {
        return;
    }

    // 只重建当前等待事务的出边。边指向持有冲突已授予锁的事务；
    // 尚未获得锁的等待请求不是实际阻塞者，因此不能作为边的终点。
    auto txn_id = txn->GetTxnId();
    waits_for_[txn_id].clear();

    auto request_iter = req_queue.req_list_iter_map_.find(txn_id);
    if (request_iter != req_queue.req_list_iter_map_.end()) {
        auto requested_mode = request_iter->second->lock_mode_;
        for (const auto &holder : req_queue.req_list_) {
            if (holder.txn_id_ == txn_id || holder.granted_ == LockMode::kNone) {
                continue;
            }

            // 排他等待者与所有持有者冲突；共享等待者只与排他持有者或正在升级的持有者冲突。
            bool conflict = requested_mode == LockMode::kExclusive;
            conflict = conflict || holder.granted_ == LockMode::kExclusive;
            conflict = conflict || holder.lock_mode_ == LockMode::kExclusive;
            if (conflict) {
                AddEdge(txn_id, holder.txn_id_);
            }
        }
    }

    if (txn->GetState() == TxnState::kAborted) {
        throw TxnAbortException(txn_id, AbortReason::kDeadlock);
    }
}

/**
 * 向等待图中加入一条依赖边。
 */
void LockManager::AddEdge(txn_id_t t1, txn_id_t t2) {
    // 事务不能等待自己；集合还能自动去重，避免多个资源重复产生同一条依赖边。
    if (t1 != t2) {
        waits_for_[t1].insert(t2);
    }
}

/**
 * 从等待图中删除一条依赖边；如果事务已经没有出边，则删除对应的空集合。
 */
void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2) {
    auto iter = waits_for_.find(t1);
    if (iter == waits_for_.end()) {
        return;
    }
    iter->second.erase(t2);
    if (iter->second.empty()) {
        waits_for_.erase(iter);
    }
}

/**
 * 使用 DFS 检查等待图中的环，并返回环中事务 ID 最大的事务作为牺牲者。
 * 对节点和邻接点排序是为了让检测结果稳定，便于复现和测试。
 */
bool LockManager::HasCycle(txn_id_t &newest_tid_in_cycle) {
    newest_tid_in_cycle = INVALID_TXN_ID;

    // 对节点排序，使找到的第一个环具有确定性，便于测试和稳定选择牺牲者。
    std::vector<txn_id_t> nodes;
    for (const auto &entry : waits_for_) {
        nodes.push_back(entry.first);
        nodes.insert(nodes.end(), entry.second.begin(), entry.second.end());
    }
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

    std::unordered_map<txn_id_t, uint8_t> color;
    std::vector<txn_id_t> path;
    std::function<bool(txn_id_t)> dfs = [&](txn_id_t current) {
        color[current] = 1;  // 灰色：当前节点仍在 DFS 递归栈中。
        path.push_back(current);

        auto edge_iter = waits_for_.find(current);
        if (edge_iter != waits_for_.end()) {
            std::vector<txn_id_t> next_nodes(edge_iter->second.begin(), edge_iter->second.end());
            std::sort(next_nodes.begin(), next_nodes.end());
            for (txn_id_t next : next_nodes) {
                if (color[next] == 0) {
                    if (dfs(next)) {
                        return true;
                    }
                } else if (color[next] == 1) {
                    auto cycle_begin = std::find(path.begin(), path.end(), next);
                    newest_tid_in_cycle = *std::max_element(cycle_begin, path.end());
                    return true;
                }
            }
        }

        path.pop_back();
        color[current] = 2;  // 黑色：当前节点及其后继已经遍历完成。
        return false;
    };

    for (txn_id_t node : nodes) {
        if (color[node] == 0 && dfs(node)) {
            return true;
        }
    }
    return false;
}

void LockManager::DeleteNode(txn_id_t txn_id) {
    // 先删除牺牲者的出边；事务中止后不应继续参与等待图。
    waits_for_.erase(txn_id);

    if (txn_mgr_ == nullptr) {
        return;
    }

    auto *txn = txn_mgr_->GetTransaction(txn_id);
    if (txn == nullptr) {
        return;
    }

    // 删除等待牺牲者所持锁的事务的入边。
    // 这里不能使用 operator[]，否则缺失的 RID 会在清理过程中被意外创建为空队列。
    auto remove_incoming_edges = [&](const std::unordered_set<RowId> &lock_set) {
        for (const auto &row_id : lock_set) {
            auto queue_iter = lock_table_.find(row_id);
            if (queue_iter == lock_table_.end()) {
                continue;
            }
            for (const auto &lock_req : queue_iter->second.req_list_) {
                if (lock_req.granted_ == LockMode::kNone) {
                    RemoveEdge(lock_req.txn_id_, txn_id);
                }
            }
        }
    };

    remove_incoming_edges(txn->GetSharedLockSet());
    remove_incoming_edges(txn->GetExclusiveLockSet());
}

/**
 * 后台死锁检测循环。
 * 检测到环后中止环中最年轻的事务，删除其等待图节点，并唤醒所有等待者，
 * 让被中止的加锁线程自行清理请求并抛出异常。
 */
void LockManager::RunCycleDetection() {
    while (enable_cycle_detection_) {
        std::this_thread::sleep_for(cycle_detection_interval_);
        if (!enable_cycle_detection_) {
            break;
        }

        std::unique_lock<std::mutex> lock(latch_);
        txn_id_t victim_id = INVALID_TXN_ID;
        if (!HasCycle(victim_id)) {
            continue;
        }

        auto *victim = txn_mgr_ == nullptr ? nullptr : txn_mgr_->GetTransaction(victim_id);
        if (victim != nullptr) {
            // 阻塞中的加锁调用会观察到该状态，清理自身请求并向调用者抛出死锁异常。
            victim->SetState(TxnState::kAborted);
            DeleteNode(victim_id);
        }

        // 唤醒所有等待者。清理等待图后，牺牲者可能正在等待一个未直接出现在
        // 当前第一条环边中的锁队列，因此不能只通知单个队列。
        for (auto &entry : lock_table_) {
            entry.second.cv_.notify_all();
        }
    }
}

/**
 * 返回等待图中的全部边，主要用于调试和单元测试。
 * 排序后返回可以避免 unordered_map/set 的遍历顺序导致测试结果不稳定。
 */
std::vector<std::pair<txn_id_t, txn_id_t>> LockManager::GetEdgeList() {
    std::vector<std::pair<txn_id_t, txn_id_t>> result;
    for (const auto &entry : waits_for_) {
        for (txn_id_t to : entry.second) {
            result.emplace_back(entry.first, to);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
