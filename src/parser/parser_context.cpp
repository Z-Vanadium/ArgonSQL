#include "parser/parser_context.h"

extern "C" {
#include "parser/minisql_lex.h"
#include "parser/parser.h"
int yyparse(void);
}

ParserContext::~ParserContext() {
  Reset();
}

ParserResult ParserContext::Parse(const std::string &sql) {
  Reset();

  YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
  if (buffer == nullptr) {
    return {nullptr, "Failed to create SQL parser buffer."};
  }

  yy_switch_to_buffer(buffer);
  buffer_ = buffer;
  MinisqlParserInit();
  active_ = true;
  yyparse();

  ParserResult result;
  if (MinisqlParserGetError()) {
    const char *message = MinisqlParserGetErrorMessage();
    result.error = message == nullptr ? "SQL parse error." : message;
    Reset();
  } else {
    result.root = MinisqlGetParserRootNode();
  }
  return result;
}

void ParserContext::Reset() {
  if (!active_) {
    return;
  }

  // 先释放语法树，再删除 lexer buffer，最后销毁 lexer 的线程局部状态。
  MinisqlParserFinish();
  yy_delete_buffer(buffer_);
  buffer_ = nullptr;
  yylex_destroy();
  active_ = false;
}
