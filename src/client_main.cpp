#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "client/client.h"

namespace {
void PrintUsage(const char *program) {
  std::cerr << "Usage: " << program << " [--host IPv4] [--port PORT] [--sql SQL]\n";
}
}  // namespace

int main(int argc, char **argv) {
  // 命令行客户端只负责连接和展示结果，SQL 的解析、规划、执行以及
  // Session 状态全部在 Server 端完成。这样客户端和数据库文件解耦，
  // 也保证多个客户端共享同一套锁、Buffer Pool 和恢复逻辑。
  std::string host = "127.0.0.1";
  uint16_t port = 6789;
  std::string one_sql;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if ((argument == "--host" || argument == "-h") && i + 1 < argc) {
      host = argv[++i];
    } else if ((argument == "--port" || argument == "-p") && i + 1 < argc) {
      const long value = std::strtol(argv[++i], nullptr, 10);
      if (value <= 0 || value > 65535) {
        PrintUsage(argv[0]);
        return 2;
      }
      port = static_cast<uint16_t>(value);
    } else if (argument == "--sql" && i + 1 < argc) {
      one_sql = argv[++i];
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  ArgonSQLClient client(host, port);
  std::string error;
  if (!client.Connect(&error)) {
    std::cerr << "Failed to connect: " << error << '\n';
    return 1;
  }

  auto execute = [&client](const std::string &sql) {
    // 一行对应一条协议请求；SQL 仍应以分号结束，因为 Server 的 parser
    // 按 MiniSQL 原有语法工作。客户端不自己拆分 SQL，避免字符串中的
    // 分号、引号和注释规则在客户端与 Server 出现两套不一致实现。
    if (sql.empty()) return true;
    ClientResponse response = client.Execute(sql);
    std::cout << response.payload;
    return !response.ShouldQuit() && response.IsSuccess();
  };

  if (!one_sql.empty()) return execute(one_sql) ? 0 : 1;

  std::string line;
  // 交互模式复用同一个 TCP 连接，因此 USE DATABASE 等会话状态可以跨
  // 多条命令保留；输入 EOF 时由析构函数关闭连接。
  while (std::cout << "argon> " && std::getline(std::cin, line)) {
    if (!execute(line)) return 1;
  }
  return 0;
}
