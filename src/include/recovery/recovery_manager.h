#ifndef MINISQL_RECOVERY_MANAGER_H
#define MINISQL_RECOVERY_MANAGER_H

#include <map>
#include <unordered_map>
#include <vector>

#include "recovery/log_rec.h"

using KvDatabase = std::unordered_map<KeyType, ValType>;
using ATT = std::unordered_map<txn_id_t, lsn_t>;

struct CheckPoint {
    lsn_t checkpoint_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase persist_data_{};

    inline void AddActiveTxn(txn_id_t txn_id, lsn_t last_lsn) { active_txns_[txn_id] = last_lsn; }

    inline void AddData(KeyType key, ValType val) { persist_data_.emplace(std::move(key), val); }
};

class RecoveryManager {
public:
    /**
     * 从检查点恢复持久化数据和检查点时仍活跃的事务。
     */
    void Init(CheckPoint &last_checkpoint) {
        log_recs_.clear();
        data_ = last_checkpoint.persist_data_;
        active_txns_ = last_checkpoint.active_txns_;
        persist_lsn_ = last_checkpoint.checkpoint_lsn_;
    }

    /**
     * 按 LSN 顺序重放日志，并同步维护活跃事务表。
     * 检查点之前的更新使用其旧值重建检查点边界，检查点之后的更新使用新值。
     */
    void RedoPhase() {
        for (const auto &[lsn, log] : log_recs_) {
            if (log == nullptr) {
                continue;
            }
            switch (log->type_) {
                case LogRecType::kBegin:
                    active_txns_[log->txn_id_] = lsn;
                    break;
                case LogRecType::kCommit:
                case LogRecType::kAbort:
                    active_txns_.erase(log->txn_id_);
                    break;
                case LogRecType::kInsert:
                    data_[log->new_key_] = log->new_val_;
                    break;
                case LogRecType::kDelete:
                    if (persist_lsn_ != INVALID_LSN && lsn <= persist_lsn_) {
                        data_[log->old_key_] = log->old_val_;
                    } else {
                        data_.erase(log->old_key_);
                    }
                    break;
                case LogRecType::kUpdate:
                    if (persist_lsn_ != INVALID_LSN && lsn <= persist_lsn_) {
                        data_[log->old_key_] = log->old_val_;
                    } else {
                        if (log->old_key_ != log->new_key_) {
                            data_.erase(log->old_key_);
                        }
                        data_[log->new_key_] = log->new_val_;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /**
     * 逆序回滚 REDO 后仍未提交的事务。
     */
    void UndoPhase() {
        for (auto iter = log_recs_.rbegin(); iter != log_recs_.rend(); ++iter) {
            const auto &log = iter->second;
            if (log == nullptr || active_txns_.find(log->txn_id_) == active_txns_.end()) {
                continue;
            }
            switch (log->type_) {
                case LogRecType::kInsert:
                    data_.erase(log->new_key_);
                    break;
                case LogRecType::kDelete:
                    data_[log->old_key_] = log->old_val_;
                    break;
                case LogRecType::kUpdate:
                    if (log->old_key_ != log->new_key_) {
                        data_.erase(log->new_key_);
                    }
                    data_[log->old_key_] = log->old_val_;
                    break;
                default:
                    break;
            }
        }
    }

    // used for test only
    void AppendLogRec(LogRecPtr log_rec) { log_recs_.emplace(log_rec->lsn_, log_rec); }

    // used for test only
    inline KvDatabase &GetDatabase() { return data_; }

private:
    std::map<lsn_t, LogRecPtr> log_recs_{};
    lsn_t persist_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase data_{};  // all data in database
};

#endif  // MINISQL_RECOVERY_MANAGER_H
