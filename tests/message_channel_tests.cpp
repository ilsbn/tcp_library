#include <tcp_helper/message_channel.hpp>

// Application-facing headers must compile without pulling in the TCP backend.
#if defined(BOOST_ASIO_VERSION) || defined(BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED)
#error message_channel.hpp must not include Boost.Asio or Boost.System
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
class MockMessageSender final : public tcp_helper::IMessageSender
{
public:
    MOCK_METHOD(bool, send, (const tcp_helper::Heartbeat&), (override));
    MOCK_METHOD(bool, send, (const tcp_helper::ShoppingCart&), (override));
    MOCK_METHOD(void, close, (), (override));
    MOCK_METHOD(bool, is_open, (), (const, override));
};

TEST(MessageChannelTest, ApplicationCanSendTypedMessagesWithMockTransport)
{
    MockMessageSender mock;
    tcp_helper::IMessageSender& sender = mock;
    const tcp_helper::Heartbeat heartbeat{1, {"alice", 42}};
    const tcp_helper::ShoppingCart cart{{"alice", 42}, {{10, 1.5F}}};
    EXPECT_CALL(mock, send(testing::An<const tcp_helper::Heartbeat&>())).WillOnce(testing::Return(true));
    EXPECT_CALL(mock, send(testing::An<const tcp_helper::ShoppingCart&>())).WillOnce(testing::Return(false));
    EXPECT_TRUE(sender.send(heartbeat));
    EXPECT_FALSE(sender.send(cart));
}
} // namespace
