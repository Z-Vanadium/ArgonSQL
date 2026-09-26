#ifndef ARGONSQL_SESSION_CONTEXT_H
#define ARGONSQL_SESSION_CONTEXT_H

#include <cstdint>
#include <string>
#include <utility>

#include "concurrency/txn.h"
#include "parser/parser_context.h"

/**
 * 一个客户端连接对应的会话状态。
 *
 * 数据库对象、Catalog 和 Buffer Pool 由 ExecuteEngine 共享；当前数据库、
 * 当前事务等信息则属于某一个客户端，必须放在 SessionContext 中，不能放
 * 在共享的 ExecuteEngine 中，否则客户端 A 的 USE DATABASE 会影响客户端 B。
 */
class SessionContext {
 public:
  explicit SessionContext(uint64_t session_id = 0) : session_id_(session_id) {}

  /** 返回服务器分配的连接 ID，便于日志和后续事务管理。 */
  uint64_t GetSessionId() const { return session_id_; }

  /** 返回该连接当前使用的数据库名称。 */
  const std::string &GetCurrentDatabase() const { return current_database_; }

  /** 执行 USE DATABASE 后更新该连接的数据库状态。 */
  void SetCurrentDatabase(std::string database) { current_database_ = std::move(database); }

  /** 删除当前数据库选择，通常在 DROP DATABASE 或连接重置时使用。 */
  void ClearCurrentDatabase() { current_database_.clear(); }

  /** 返回该连接正在使用的事务；事务生命周期由后续 TxnManager 管理。 */
  Txn *GetTransaction() const { return transaction_; }

  /** 设置该连接关联的事务对象。 */
  void SetTransaction(Txn *transaction) { transaction_ = transaction; }

  /** 返回该连接独占的 parser，避免不同连接共享 lexer/parser 状态。 */
  ParserContext *GetParser() { return &parser_; }

 private:
  uint64_t session_id_;
  std::string current_database_;
  Txn *transaction_{nullptr};
  ParserContext parser_;
};

#endif  // ARGONSQL_SESSION_CONTEXT_H
