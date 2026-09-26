#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>

#include "server/tcp_server.h"

namespace {
void PrintUsage(const char *program) {
  std::cerr << "Usage: " << program << " [--host IPv4] [--port PORT] [--workers N]\n";
}
}  // namespace

int main(int argc, char **argv) {
  // 默认只监听本机，避免未配置认证和加密时直接暴露到网络。
  std::string host = "127.0.0.1";
  uint16_t port = 6789;
  size_t worker_count = 4;

  // Server 目前只需要两个配置项；非法参数直接打印使用说明并退出，
  // 避免带着不确定的端口或地址启动。
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
    } else if (argument == "--workers" && i + 1 < argc) {
      const long value = std::strtol(argv[++i], nullptr, 10);
      if (value <= 0 || value > 128) {
        PrintUsage(argv[0]);
        return 2;
      }
      worker_count = static_cast<size_t>(value);
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  // TcpServer 的生命周期覆盖整个进程；Run 返回通常意味着监听失败
  // 或服务被 Stop，返回值直接作为进程退出码。
  TcpServer server(host, port, worker_count);
  return server.Run();
}
