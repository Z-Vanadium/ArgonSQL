#include <cstdio>
#include <fstream>
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
  if (buffer == nullptr) {
    return {};
  }

  yy_switch_to_buffer(buffer);
  MinisqlParserInit();
  yyparse();
  EXPECT_EQ(MinisqlParserGetError(), 0) << MinisqlParserGetErrorMessage();

  ExecuteResult result;
  if (!MinisqlParserGetError()) {
    result = engine->Execute(MinisqlGetParserRootNode(), session);
  }

  MinisqlParserFinish();
  yy_delete_buffer(buffer);
  yylex_destroy();
  return result;
}
}  // namespace

TEST(ExecuteEnginePersistenceTest, ReloadsDatabaseAfterEngineRestart) {
  const std::string database_name = "argonsql_persistence_test";
  const std::string database_path = "./databases/" + database_name;
  std::remove(database_path.c_str());

  // 第一个 ExecuteEngine 模拟 Server 第一次启动，CREATE DATABASE 会创建
  // 数据库文件并把 DBStorageEngine 放入当前进程的 dbs_ 中。
  {
    ExecuteEngine engine;
    SessionContext session(1);
    const ExecuteResult result = ExecuteSql(&engine, &session, "create database argonsql_persistence_test;");
    ASSERT_EQ(result.status, DB_SUCCESS);
    ASSERT_TRUE(std::ifstream(database_path).good());
  }

  // 第一个引擎析构模拟 Server 停止。第二个 ExecuteEngine 模拟 Server
  // 重启，构造函数应该从 databases/ 扫描并重新打开该数据库。
  {
    ExecuteEngine restarted_engine;
    SessionContext session(2);

    const ExecuteResult show_result = ExecuteSql(&restarted_engine, &session, "show databases;");
    ASSERT_EQ(show_result.status, DB_SUCCESS);
    EXPECT_NE(show_result.output.find(database_name), std::string::npos);

    const ExecuteResult use_result = ExecuteSql(&restarted_engine, &session, "use argonsql_persistence_test;");
    EXPECT_EQ(use_result.status, DB_SUCCESS);
    EXPECT_EQ(session.GetCurrentDatabase(), database_name);
  }

  EXPECT_EQ(std::remove(database_path.c_str()), 0);
}
