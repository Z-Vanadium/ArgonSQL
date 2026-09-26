#include <future>
#include <string>
#include <vector>

#include "executor/execute_engine.h"
#include "executor/session_context.h"
#include "gtest/gtest.h"
#include "parser/parser_context.h"

/**
 * 验证多个连接可以同时经过“解析 -> 执行 -> 结果捕获”链路。
 *
 * 这个测试特意共享同一个 ExecuteEngine，模拟 Server 中所有 worker
 * 共享数据库实例的实际部署方式；每个请求仍然拥有独立 SessionContext
 * 和 ParserContext，因此结果不能串到其它连接。
 */
TEST(ExecuteEngineConcurrencyTest, ExecutesIndependentRequestsConcurrently) {
  ExecuteEngine engine;
  constexpr int kRequestCount = 16;
  std::vector<std::future<ExecuteResult>> requests;
  requests.reserve(kRequestCount);

  for (int i = 0; i < kRequestCount; ++i) {
    requests.emplace_back(std::async(std::launch::async, [&engine] {
      SessionContext session;
      ParserContext parser;
      ParserResult parsed = parser.Parse("show databases;");
      EXPECT_TRUE(parsed.IsSuccess());
      if (!parsed.IsSuccess()) {
        return ExecuteResult{};
      }
      ExecuteResult result = engine.Execute(parsed.root, &session);
      parser.Reset();
      return result;
    }));
  }

  for (auto &request : requests) {
    ExecuteResult result = request.get();
    EXPECT_TRUE(result.IsSuccess());
    EXPECT_FALSE(result.output.empty());
  }
}
