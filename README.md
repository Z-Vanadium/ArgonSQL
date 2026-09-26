# ArgonSQL

ArgonSQL 是一个基于 C++17 和 Linux 的多用户关系型数据库项目。
项目从 MiniSQL 课程设计演进而来，提供页式存储、Buffer Pool、B+ Tree 索引、SQL 执行器和事务基础设施，并逐步演进为客户端—服务器数据库。

## 项目特点

- Linux 环境下运行的多用户数据库服务器
- 基于 TCP 的客户端连接
- SQL Parser、Planner 和 Executor 执行链路
- 页式数据库文件和 Buffer Pool
- Table Heap 记录存储
- B+ Tree 主键及二级索引
- 事务、锁管理和 WAL 崩溃恢复
- 单元测试、并发测试和性能 Benchmark

## 系统架构

```text
TCP Client
  ↓
epoll Reactor / Connection
  ↓
SQL Worker Pool
  ↓
Planner
  ↓
ExecuteEngine / Executor
  ↓
Catalog / TableHeap / B+ Tree
  ↓
BufferPool / DiskManager
```

Server 进程统一管理 Catalog、Buffer Pool、事务、锁和恢复组件；每个客户端连接拥有独立的 Session 和事务上下文。

## 代码结构

```text
src/parser/       SQL 词法与语法解析
src/planner/      查询计划生成
src/executor/     SQL 执行器
src/catalog/      表和索引元数据
src/record/       Row、Field、Schema
src/storage/      DiskManager、TableHeap
src/buffer/       Buffer Pool
src/index/        B+ Tree 索引
src/concurrency/  事务与锁管理
src/recovery/     WAL 与崩溃恢复
test/             GoogleTest 测试
docs/             项目文档
```

## 客户端协议与连接

Server 使用 `ARGONSQL/1` 长度前缀协议，客户端可以在一条持久 TCP 连接上连续发送多条 SQL。响应明确携带 `OK`、`ERROR` 或 `QUIT` 状态和 payload 字节长度，不依赖换行或连接关闭判断结果边界。

```text
ARGONSQL/1 REQUEST <bytes>\n<SQL>
ARGONSQL/1 RESPONSE <OK|ERROR|QUIT> <bytes>\n<result>
```

每个连接拥有独立 Session；epoll Reactor 负责管理大量非阻塞连接，SQL 线程池只执行完整 SQL，不会被空闲连接占用。协议细节见 [docs/protocol.md](docs/protocol.md)。

## 构建与测试

环境要求：

- Linux
- GCC 11+
- C++17
- CMake 3.16+
- GoogleTest

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

启动 Server 后可使用命令行客户端：

```bash
./build/bin/argonsql_server --host 127.0.0.1 --port 6789 --workers 4
./build/bin/argonsql_client --host 127.0.0.1 --port 6789
./build/bin/argonsql_client --sql 'show databases;'
```

## 项目定位

一个用于学习和展示数据库内核、C++ 系统编程、Linux 网络编程、并发控制、持久化和性能优化的个人数据库项目。
