#ifndef MINISQL_EXECUTE_ENGINE_H
#define MINISQL_EXECUTE_ENGINE_H

#include <memory>
#include <string>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <ostream>

#include "common/dberr.h"
#include "common/instance.h"
#include "concurrency/txn.h"
#include "executor/execute_context.h"
#include "executor/executors/abstract_executor.h"
#include "executor/plans/abstract_plan.h"
#include "record/row.h"

class SessionContext;

/**
 * 一条 SQL 的执行结果。
 *
 * 执行引擎原先只返回 dberr_t，并且把查询结果直接写到 std::cout。
 * 服务器无法从标准输出中可靠地区分不同客户端，因此这里同时返回：
 * 1. status：机器可判断的执行状态；
 * 2. output：发送给 CLI 或网络客户端的文本结果。
 */
struct ExecuteResult {
  dberr_t status{DB_FAILED};
  std::string output;

  /** 判断该请求是否正常完成。DB_QUIT 由 ShouldQuit 单独处理。 */
  bool IsSuccess() const { return status == DB_SUCCESS; }

  /** 判断客户端是否要求关闭当前连接。 */
  bool ShouldQuit() const { return status == DB_QUIT; }
};

extern "C" {
#include "parser/parser.h"
};

/**
 * ExecuteEngine
 */
class ExecuteEngine {
 public:
  ExecuteEngine();

  ~ExecuteEngine() {
    for (auto it : dbs_) {
      delete it.second;
    }
  }

  /**
   * 兼容原有 CLI 和 execfile 的执行接口。
   */
  dberr_t Execute(pSyntaxNode ast);

  /**
   * 面向客户端会话的执行接口。
   *
   * session 中保存当前数据库等连接级状态；ExecuteEngine 中的数据库实例
   * 仍然由所有会话共享。返回值包含状态和文本结果，Server 可以直接发送
   * result.output，而不需要依赖标准输出。
   */
  ExecuteResult Execute(pSyntaxNode ast, SessionContext *session);

  dberr_t ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                      ExecuteContext *exec_ctx);

  void ExecuteInformation(dberr_t result);

  /** 请求级输出流；Server 请求不会再重定向进程级 std::cout。 */
  std::ostream &Output();

  /** 返回当前请求所属会话的数据库；兼容旧 CLI 时回退到 current_db_。 */
  const std::string &CurrentDatabase() const;

  /** 更新当前请求所属会话的数据库；兼容旧 CLI 时更新 current_db_。 */
  void SetCurrentDatabase(std::string database);

 private:
  static std::unique_ptr<AbstractExecutor> CreateExecutor(ExecuteContext *exec_ctx, const AbstractPlanNodeRef &plan);

  dberr_t ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context);

  dberr_t ExecuteQuit(pSyntaxNode ast, ExecuteContext *context);

 private:
  std::unordered_map<std::string, DBStorageEngine *> dbs_; /** all opened databases */
  std::string current_db_;                                 /** current database */
  /** 保护共享数据库注册表；读请求共享锁，DDL/DML 使用排他锁。 */
  mutable std::shared_mutex dbs_latch_;
};

#endif  // MINISQL_EXECUTE_ENGINE_H
