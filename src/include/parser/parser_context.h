#ifndef ARGONSQL_PARSER_CONTEXT_H
#define ARGONSQL_PARSER_CONTEXT_H

#include <string>

#include "parser/syntax_tree.h"

extern "C" {
#include "parser/minisql_lex.h"
}

struct ParserResult {
  pSyntaxNode root{nullptr};
  std::string error;

  bool IsSuccess() const { return error.empty() && root != nullptr; }
};

/**
 * 一个连接独占的 SQL parser 上下文。
 *
 * ParserContext 管理一次 SQL 请求对应的 lexer buffer、语法树和错误信息。
 * 生成的 flex/bison 状态已经改为线程局部变量，因此不同连接的
 * ParserContext 可以在不同 worker 中并行使用。
 */
class ParserContext {
 public:
  ParserContext() = default;
  ~ParserContext();

  ParserContext(const ParserContext &) = delete;
  ParserContext &operator=(const ParserContext &) = delete;

  ParserResult Parse(const std::string &sql);
  void Reset();

 private:
  bool active_{false};
  YY_BUFFER_STATE buffer_{nullptr};
};

#endif  // ARGONSQL_PARSER_CONTEXT_H
