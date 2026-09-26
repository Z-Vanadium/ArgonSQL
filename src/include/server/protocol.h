#ifndef ARGONSQL_PROTOCOL_H
#define ARGONSQL_PROTOCOL_H

#include <cstddef>
#include <string>

namespace ArgonSQLProtocol {

/**
 * 当前客户端—服务器协议版本。
 *
 * 版本号放在每一帧的头部，而不是只在连接建立时发送一次，原因是：
 * 1. 新旧客户端连接到同一个端口时可以快速识别协议版本；
 * 2. 抓包和日志中可以直接判断一段数据属于哪一版协议；
 * 3. 后续修改头部字段时可以增加 ARGONSQL/2，而不必猜测旧格式。
 */
constexpr const char *kVersion = "ARGONSQL/1";

/**
 * 响应状态与 SQL 执行状态一一对应。
 *
 * 状态和文本 payload 分离：客户端不需要通过匹配“错误字符串”判断
 * 成功与否，同时 payload 仍然可以原样承载 ResultWriter 的多行结果。
 */
enum class ResponseStatus { kOk, kError, kQuit };

/**
 * 解析一行 REQUEST 头，返回后续 payload 的字节数。
 *
 * 头部只描述长度，不承载 SQL 内容；这样 SQL 中出现换行、分号甚至
 * 空字节时，也不会被错误地当成下一条请求的边界。
 */
bool ParseRequestHeader(const std::string &header, size_t *payload_size);

/**
 * 生成带长度的 RESPONSE 头；payload 可以包含换行和分号。
 * 长度统一按字节计算，不能按 UTF-8 字符数计算，否则中文 SQL 或结果
 * 会导致接收端少读或多读，进而破坏后续帧的边界。
 */
std::string BuildResponseHeader(ResponseStatus status, size_t payload_size);

/**
 * 通过完整写入循环发送一个协议响应。
 * send() 允许只发送部分数据，因此必须循环；MSG_NOSIGNAL 则避免客户端
 * 已断开时给 Server 进程发送 SIGPIPE。
 */
bool SendResponse(int fd, ResponseStatus status, const std::string &payload);

/** 客户端发送一条带长度的 SQL 请求。 */
bool SendRequest(int fd, const std::string &sql);

/**
 * 客户端读取一条完整响应。
 * 该函数先读到头部换行，再依据头部长度精确读取 payload；不能使用
 * “读到换行”或“本次 recv 返回多少字节”作为完整响应判断条件。
 */
bool ReceiveResponse(int fd, ResponseStatus *status, std::string *payload);

}  // namespace ArgonSQLProtocol

#endif  // ARGONSQL_PROTOCOL_H
