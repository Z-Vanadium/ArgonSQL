#include <future>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "server/protocol.h"

TEST(ArgonSQLProtocolTest, ParsesLengthAndPreservesResponsePayload) {
  size_t payload_size = 0;
  ASSERT_TRUE(ArgonSQLProtocol::ParseRequestHeader("ARGONSQL/1 REQUEST 17", &payload_size));
  EXPECT_EQ(payload_size, 17U);
  EXPECT_FALSE(ArgonSQLProtocol::ParseRequestHeader("ARGONSQL/1 REQUEST nope", &payload_size));
  EXPECT_FALSE(ArgonSQLProtocol::ParseRequestHeader("ARGONSQL/2 REQUEST 1", &payload_size));
}

TEST(ArgonSQLProtocolTest, ExchangesFramedResponseOverSocketPair) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

  const std::string payload = "line 1\nline 2;\n";
  auto sender = std::async(std::launch::async, [&] {
    const bool sent = ArgonSQLProtocol::SendResponse(sockets[0], ArgonSQLProtocol::ResponseStatus::kOk, payload);
    close(sockets[0]);
    return sent;
  });

  ArgonSQLProtocol::ResponseStatus status{};
  std::string received;
  ASSERT_TRUE(ArgonSQLProtocol::ReceiveResponse(sockets[1], &status, &received));
  close(sockets[1]);
  EXPECT_TRUE(sender.get());
  EXPECT_EQ(status, ArgonSQLProtocol::ResponseStatus::kOk);
  EXPECT_EQ(received, payload);
}
