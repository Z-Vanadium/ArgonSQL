#include <future>
#include <vector>

#include "gtest/gtest.h"
#include "parser/parser_context.h"

TEST(ParserContextTest, ParsesConcurrentlyWithIndependentState) {
  std::vector<std::future<bool>> futures;
  for (int i = 0; i < 8; ++i) {
    futures.emplace_back(std::async(std::launch::async, [] {
      ParserContext parser;
      const ParserResult result = parser.Parse("show databases;");
      const bool success = result.IsSuccess();
      parser.Reset();
      return success;
    }));
  }

  for (auto &future : futures) {
    EXPECT_TRUE(future.get());
  }
}
