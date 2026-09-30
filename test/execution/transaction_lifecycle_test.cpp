#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "executor/execute_engine.h"
#include "executor/session_context.h"
#include "gtest/gtest.h"

extern "C" {
#include "parser/minisql_lex.h"
#include "parser/parser.h"
int yyparse(void);
}

namespace {
ExecuteResult ExecuteSql(ExecuteEngine *engine, SessionContext *session, const std::string &sql) {
  YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
  EXPECT_NE(buffer, nullptr);
  if (buffer == nullptr) return {};
  yy_switch_to_buffer(buffer);
  MinisqlParserInit();
  yyparse();
  EXPECT_EQ(MinisqlParserGetError(), 0) << MinisqlParserGetErrorMessage();
  ExecuteResult result;
  if (MinisqlParserGetError() == 0) result = engine->Execute(MinisqlGetParserRootNode(), session);
  MinisqlParserFinish();
  yy_delete_buffer(buffer);
  yylex_destroy();
  return result;
}
}  // namespace

TEST(TransactionLifecycleTest, SessionCommitWritesWalAndClearsTransaction) {
  const std::string name = "argonsql_transaction_lifecycle_test";
  const std::string db_path = "./databases/" + name;
  const std::string wal_path = db_path + ".wal";
  std::remove(db_path.c_str());
  std::remove(wal_path.c_str());

  {
    ExecuteEngine engine;
    SessionContext session(100);
    ASSERT_EQ(ExecuteSql(&engine, &session, "create database argonsql_transaction_lifecycle_test;" ).status,
              DB_SUCCESS);
    ASSERT_EQ(ExecuteSql(&engine, &session, "use argonsql_transaction_lifecycle_test;").status, DB_SUCCESS);
    ASSERT_EQ(ExecuteSql(&engine, &session, "begin;").status, DB_SUCCESS);
    ASSERT_NE(session.GetTransaction(), nullptr);
    ASSERT_EQ(ExecuteSql(&engine, &session, "commit;").status, DB_SUCCESS);
    ASSERT_EQ(session.GetTransaction(), nullptr);
  }

  std::ifstream wal(wal_path);
  ASSERT_TRUE(wal.good());
  std::stringstream contents;
  contents << wal.rdbuf();
  EXPECT_NE(contents.str().find("BEGIN"), std::string::npos);
  EXPECT_NE(contents.str().find("COMMIT"), std::string::npos);

  std::remove(db_path.c_str());
  std::remove(wal_path.c_str());
}

TEST(TransactionLifecycleTest, SessionDisconnectAutomaticallyAborts) {
  const std::string name = "argonsql_transaction_disconnect_test";
  const std::string db_path = "./databases/" + name;
  const std::string wal_path = db_path + ".wal";
  std::remove(db_path.c_str());
  std::remove(wal_path.c_str());

  {
    ExecuteEngine engine;
    SessionContext session(101);
    ASSERT_EQ(ExecuteSql(&engine, &session, "create database argonsql_transaction_disconnect_test;").status,
              DB_SUCCESS);
    ASSERT_EQ(ExecuteSql(&engine, &session, "use argonsql_transaction_disconnect_test;").status, DB_SUCCESS);
    ASSERT_EQ(ExecuteSql(&engine, &session, "begin;").status, DB_SUCCESS);
    ASSERT_NE(session.GetTransaction(), nullptr);
    // session 离开作用域时触发 RAII：事务管理器写 ABORT 并释放全部锁。
  }

  std::ifstream wal(wal_path);
  ASSERT_TRUE(wal.good());
  std::stringstream contents;
  contents << wal.rdbuf();
  EXPECT_NE(contents.str().find("ABORT"), std::string::npos);

  std::remove(db_path.c_str());
  std::remove(wal_path.c_str());
}
