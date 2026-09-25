#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};

// used for testing only
using KeyType = std::string;
using ValType = int32_t;

/**
 * TODO: Student Implement
 */
struct LogRec {
    LogRec() = default;

    LogRecType type_{LogRecType::kInvalid};
    txn_id_t txn_id_{INVALID_TXN_ID};
    lsn_t lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};

    // 测试用的键值负载。更新日志同时保存更新前后的值，便于 REDO/UNDO。
    KeyType old_key_{};
    ValType old_val_{};
    KeyType new_key_{};
    ValType new_val_{};

    /* used for testing only */
    static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
    static lsn_t next_lsn_;
};

std::unordered_map<txn_id_t, lsn_t> LogRec::prev_lsn_map_ = {};
lsn_t LogRec::next_lsn_ = 0;

typedef std::shared_ptr<LogRec> LogRecPtr;

/**
 * TODO: Student Implement
 */
static LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kInsert;
    log->txn_id_ = txn_id;
    log->new_key_ = std::move(ins_key);
    log->new_val_ = ins_val;
    auto prev = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = prev == LogRec::prev_lsn_map_.end() ? INVALID_LSN : prev->second;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

/**
 * TODO: Student Implement
 */
static LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kDelete;
    log->txn_id_ = txn_id;
    log->old_key_ = std::move(del_key);
    log->old_val_ = del_val;
    auto prev = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = prev == LogRec::prev_lsn_map_.end() ? INVALID_LSN : prev->second;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

/**
 * TODO: Student Implement
 */
static LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kUpdate;
    log->txn_id_ = txn_id;
    log->old_key_ = std::move(old_key);
    log->old_val_ = old_val;
    log->new_key_ = std::move(new_key);
    log->new_val_ = new_val;
    auto prev = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = prev == LogRec::prev_lsn_map_.end() ? INVALID_LSN : prev->second;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

/**
 * TODO: Student Implement
 */
static LogRecPtr CreateBeginLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kBegin;
    log->txn_id_ = txn_id;
    log->prev_lsn_ = INVALID_LSN;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

/**
 * TODO: Student Implement
 */
static LogRecPtr CreateCommitLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kCommit;
    log->txn_id_ = txn_id;
    auto prev = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = prev == LogRec::prev_lsn_map_.end() ? INVALID_LSN : prev->second;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_.erase(txn_id);
    return log;
}

/**
 * TODO: Student Implement
 */
static LogRecPtr CreateAbortLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kAbort;
    log->txn_id_ = txn_id;
    auto prev = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = prev == LogRec::prev_lsn_map_.end() ? INVALID_LSN : prev->second;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_.erase(txn_id);
    return log;
}

#endif  // MINISQL_LOG_REC_H
