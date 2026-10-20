#include <tcp_helper/tcp.hpp>
#include <tcp_helper/messages.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using tcp_helper::ConnectionPtr;
using tcp_helper::ByteOrder;
using tcp_helper::DataBuffer;
using tcp_helper::ErrorCode;
using testing::_;

namespace
{

DataBuffer make_message(std::string_view text)
{
    return DataBuffer{std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}};
}

class RecordingEvents : public tcp_helper::RawTcpEvents
{
public:
    void on_connected(ConnectionPtr connection) override
    {
        connections.push_back(connection);
    }
    void on_message(ConnectionPtr, DataBuffer message) override
    {
        messages.push_back(std::move(message));
    }
    void on_disconnected(ConnectionPtr, ErrorCode reason) override
    {
        disconnects.push_back(reason);
    }
    void on_error(std::string_view, ErrorCode error) override
    {
        errors.push_back(error);
    }

    std::vector<ConnectionPtr> connections;
    std::vector<DataBuffer> messages;
    std::vector<ErrorCode> disconnects;
    std::vector<ErrorCode> errors;
};

class EchoEvents : public RecordingEvents
{
public:
    void on_message(ConnectionPtr connection, DataBuffer message) override
    {
        RecordingEvents::on_message(connection, message);
        EXPECT_TRUE(connection->send(std::move(message)));
    }
};

class ValidatingEvents : public RecordingEvents
{
public:
    void on_message(ConnectionPtr connection, DataBuffer message) override
    {
        try
        {
            decoded.push_back(tcp_helper::decode_message(message));
            RecordingEvents::on_message(connection, std::move(message));
        }
        catch (const std::invalid_argument&)
        {
            ++invalid_messages;
            connection->close();
        }
    }

    std::vector<tcp_helper::DecodedMessage> decoded;
    std::size_t invalid_messages = 0;
};

template <class Owner> class DestroyOwnerEvents final : public RecordingEvents
{
public:
    explicit DestroyOwnerEvents(std::unique_ptr<Owner>& owner) : owner_(owner)
    {
    }
    void on_connected(ConnectionPtr connection) override
    {
        RecordingEvents::on_connected(std::move(connection));
        owner_.reset();
    }

private:
    std::unique_ptr<Owner>& owner_;
};

class MockEvents : public tcp_helper::RawTcpEvents
{
public:
    MOCK_METHOD(void, on_connected, (ConnectionPtr), (override));
    MOCK_METHOD(void, on_message, (ConnectionPtr, DataBuffer), (override));
    MOCK_METHOD(void, on_disconnected, (ConnectionPtr, ErrorCode), (override));
    MOCK_METHOD(void, on_error, (std::string_view, ErrorCode), (override));
};

class TcpTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_FALSE(server.start("127.0.0.1", 0));
    }
    void TearDown() override
    {
        client.stop();
        server.stop();
        context.restart();
        context.run_for(100ms);
    }

    template <class Predicate> bool until(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
        {
            context.restart();
            context.run_for(5ms);
        }
        return predicate();
    }

    bool connect()
    {
        client.connect("127.0.0.1", server.port());
        return until(
            [this]
            {
                return !client_events->connections.empty() && !server_events->connections.empty();
            });
    }

    boost::asio::io_context context;
    std::shared_ptr<EchoEvents> server_events = std::make_shared<EchoEvents>();
    std::shared_ptr<RecordingEvents> client_events = std::make_shared<RecordingEvents>();
    tcp_helper::TcpServer server{context, server_events};
    tcp_helper::TcpClient client{context, client_events};
};

// Catches missing framing, overlapping writes, and loss of empty/binary payloads.
TEST_F(TcpTest, PreservesOrderedBinaryAndEmptyMessages)
{
    ASSERT_TRUE(connect());
    const std::vector<DataBuffer> expected{make_message("first"), DataBuffer{},
                                           make_message(std::string_view("a\0b", 3)),
                                           DataBuffer{std::vector<std::uint8_t>(128 * 1024, 'x')}};
    for (const auto& message : expected)
    {
        ASSERT_TRUE(client_events->connections.front()->send(message));
    }
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->messages.size() == 4;
        }));
    EXPECT_EQ(client_events->messages, expected);
    EXPECT_EQ(server_events->messages, expected);
}

TEST_F(TcpTest, SendsCompleteStructuredBufferRegardlessOfReadPosition)
{
    ASSERT_TRUE(connect());
    DataBuffer outgoing;
    outgoing.write_uint32(0x12345678);
    outgoing.write_int16(-2, ByteOrder::little_endian);
    outgoing.write_uint64(0x0123456789abcdef, ByteOrder::little_endian);
    ASSERT_EQ(outgoing.read_uint32(), 0x12345678u);
    ASSERT_TRUE(client_events->connections.front()->send(std::move(outgoing)));
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->messages.size() == 1;
        }));
    auto received = std::move(client_events->messages.front());
    EXPECT_EQ(received.read_position(), 0u);
    EXPECT_THAT(received.bytes(), testing::ElementsAre(0x12, 0x34, 0x56, 0x78, 0xfe, 0xff, 0xef, 0xcd, 0xab, 0x89, 0x67,
                                                       0x45, 0x23, 0x01));
    EXPECT_EQ(received.read_uint32(), 0x12345678u);
    EXPECT_EQ(received.read_int16(ByteOrder::little_endian), -2);
    EXPECT_EQ(received.read_uint64(ByteOrder::little_endian), 0x0123456789abcdefull);
    EXPECT_EQ(received.remaining(), 0u);
}

TEST_F(TcpTest, ExchangesHeartbeatAndShoppingCartWithValidChecksums)
{
    ASSERT_TRUE(connect());
    const std::array<tcp_helper::MessageBody, 2> expected{
        tcp_helper::Heartbeat{1001, {"alice", 42}},
        tcp_helper::ShoppingCart{{"alice", 42}, {{10, 12.5F}, {20, 3.25F}}}};
    for (const auto& body : expected)
    {
        ASSERT_TRUE(client_events->connections.front()->send(tcp_helper::encode_message(body)));
    }
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->messages.size() == 2;
        }));
    ASSERT_EQ(server_events->messages.size(), 2);
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto at_server = tcp_helper::decode_message(server_events->messages[index]);
        const auto at_client = tcp_helper::decode_message(client_events->messages[index]);
        EXPECT_EQ(at_server.m_body, expected[index]);
        EXPECT_EQ(at_client.m_body, expected[index]);
        EXPECT_EQ(at_server.m_checksum, at_client.m_checksum);
    }
}

TEST_F(TcpTest, RawReceiverCanChooseToRejectAndClose)
{
    auto events = std::make_shared<ValidatingEvents>();
    tcp_helper::TcpServer typed_server{context, events};
    ASSERT_FALSE(typed_server.start("127.0.0.1", 0));
    client.connect("127.0.0.1", typed_server.port());
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->connections.size() == 1;
        }));
    const auto valid = tcp_helper::encode_message(tcp_helper::Heartbeat{1, {"alice", 42}});
    auto corrupted = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    corrupted[tcp_helper::message_header_size] ^= 1;
    ASSERT_TRUE(client_events->connections.front()->send(DataBuffer{std::move(corrupted)}));
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->disconnects.size() == 1;
        }));
    EXPECT_EQ(events->invalid_messages, 1u);
    EXPECT_TRUE(events->decoded.empty());
    EXPECT_TRUE(events->messages.empty());
    typed_server.stop();
}

// Catches one-client-only accept loops and incorrect routing between sessions.
TEST_F(TcpTest, ServesMultipleClientsIndependently)
{
    ASSERT_TRUE(connect());
    auto second_events = std::make_shared<RecordingEvents>();
    tcp_helper::TcpClient second{context, second_events};
    second.connect("127.0.0.1", server.port());
    ASSERT_TRUE(until(
        [&second_events]
        {
            return !second_events->connections.empty();
        }));
    ASSERT_TRUE(client_events->connections.front()->send(make_message("one")));
    ASSERT_TRUE(second_events->connections.front()->send(make_message("two")));
    ASSERT_TRUE(until(
        [this, &second_events]
        {
            return client_events->messages.size() == 1 && second_events->messages.size() == 1;
        }));
    EXPECT_EQ(client_events->messages.front(), make_message("one"));
    EXPECT_EQ(second_events->messages.front(), make_message("two"));
    second.stop();
}

// Catches duplicate callbacks and operations that continue after close.
TEST_F(TcpTest, CloseIsIdempotentAndRejectsFurtherWrites)
{
    ASSERT_TRUE(connect());
    auto connection = client_events->connections.front();
    connection->close();
    connection->close();
    EXPECT_FALSE(connection->is_open());
    EXPECT_FALSE(connection->send(make_message("late")));
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->disconnects.size() == 1;
        }));
    ASSERT_EQ(client_events->disconnects.size(), 1);
    EXPECT_EQ(client_events->disconnects.front(), boost::asio::error::operation_aborted);
}

// Catches retaining only the listening socket when stopping a server.
TEST_F(TcpTest, ServerStopClosesAcceptedConnections)
{
    ASSERT_TRUE(connect());
    server.stop();
    server.stop();
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->disconnects.size() == 1;
        }));
    EXPECT_FALSE(client_events->connections.front()->is_open());
    EXPECT_EQ(server_events->disconnects.size(), 1);
}

// Catches incorrect treatment of TCP read boundaries as message boundaries.
TEST_F(TcpTest, ReassemblesFragmentedAndCoalescedFrames)
{
    boost::asio::ip::tcp::socket raw{context};
    raw.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->connections.size() == 1;
        }));
    const std::array<unsigned char, 2> prefix{0, 0};
    boost::asio::write(raw, boost::asio::buffer(prefix));
    context.restart();
    context.run_for(10ms);
    EXPECT_TRUE(server_events->messages.empty());
    const std::array<unsigned char, 11> rest{0, 3, 'a', 'b', 'c', 0, 0, 0, 2, 'd', 'e'};
    boost::asio::write(raw, boost::asio::buffer(rest));
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 2;
        }));
    EXPECT_THAT(server_events->messages, testing::ElementsAre(make_message("abc"), make_message("de")));
}

// Catches allocating an unchecked peer-provided payload length.
TEST_F(TcpTest, RejectsOversizedIncomingFrameBeforeReadingPayload)
{
    boost::asio::ip::tcp::socket raw{context};
    raw.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
    const std::array<unsigned char, 4> oversized{0x7f, 0xff, 0xff, 0xff};
    boost::asio::write(raw, boost::asio::buffer(oversized));
    ASSERT_TRUE(until(
        [this]
        {
            return !server_events->disconnects.empty();
        }));
    EXPECT_EQ(server_events->disconnects.front(), boost::asio::error::message_size);
    EXPECT_TRUE(server_events->messages.empty());
}

// Catches forgetting the outbound limits while retaining a valid connection.
TEST_F(TcpTest, RejectsOversizedOutgoingMessage)
{
    ASSERT_TRUE(connect());
    auto connection = client_events->connections.front();
    EXPECT_FALSE(connection->send(DataBuffer{std::vector<std::uint8_t>(1024 * 1024 + 1, 'x')}));
    EXPECT_TRUE(connection->send(make_message("still open")));
    ASSERT_TRUE(until(
        [this]
        {
            return !client_events->messages.empty();
        }));
    EXPECT_EQ(client_events->messages.front(), make_message("still open"));
}

TEST(TcpFailures, ReportsConnectionRefusalThroughMockObserver)
{
    boost::asio::io_context context;
    boost::asio::ip::tcp::acceptor reserved{context, {boost::asio::ip::tcp::v4(), 0}};
    const auto port = reserved.local_endpoint().port();
    reserved.close();
    auto events = std::make_shared<testing::StrictMock<MockEvents>>();
    EXPECT_CALL(*events, on_error("connect", testing::Ne(ErrorCode{}))).Times(1);
    tcp_helper::TcpClient client{context, events};
    client.connect("127.0.0.1", port);
    context.run_for(3s);
    client.stop();
}

TEST(TcpFailures, CancelPendingConnectSuppressesCallbacks)
{
    boost::asio::io_context context;
    auto events = std::make_shared<testing::StrictMock<MockEvents>>();
    tcp_helper::TcpClient client{context, events};
    client.connect("localhost", 1);
    client.stop();
    context.run_for(3s);
}

TEST(TcpFailures, InvalidBindAddressReturnsError)
{
    boost::asio::io_context context;
    auto events = std::make_shared<testing::StrictMock<MockEvents>>();
    tcp_helper::TcpServer server{context, events};
    EXPECT_TRUE(server.start("invalid-address", 0));
}

TEST_F(TcpTest, EnforcesQueueLimitAndResumesAfterWritesComplete)
{
    auto limited_events = std::make_shared<RecordingEvents>();
    tcp_helper::TcpClient limited{context, limited_events, {8, 12}};
    limited.connect("127.0.0.1", server.port());
    ASSERT_TRUE(until(
        [&limited_events]
        {
            return !limited_events->connections.empty();
        }));
    auto connection = limited_events->connections.front();
    ASSERT_TRUE(connection->send(make_message("12345678")));
    EXPECT_FALSE(connection->send(make_message("x")));
    ASSERT_TRUE(until(
        [&limited_events]
        {
            return limited_events->messages.size() == 1;
        }));
    ASSERT_TRUE(connection->send(make_message("again")));
    ASSERT_TRUE(until(
        [&limited_events]
        {
            return limited_events->messages.size() == 2;
        }));
    EXPECT_THAT(limited_events->messages, testing::ElementsAre(make_message("12345678"), make_message("again")));
    limited.stop();
}

TEST_F(TcpTest, ClosingDuringWriteKeepsPendingBuffersAlive)
{
    ASSERT_TRUE(connect());
    auto connection = client_events->connections.front();
    ASSERT_TRUE(connection->send(DataBuffer{std::vector<std::uint8_t>(1024 * 1024, 'x')}));
    // Start the first posted write before closing, with another submission still pending.
    boost::asio::post(context,
                      [connection]
                      {
                          connection->close();
                      });
    ASSERT_TRUE(connection->send(make_message("pending")));
    ASSERT_TRUE(until(
        [this]
        {
            return !server_events->disconnects.empty();
        }));
    EXPECT_EQ(client_events->disconnects.size(), 1);
    EXPECT_FALSE(connection->is_open());
}

TEST(TcpFailures, RejectsInvalidLimits)
{
    boost::asio::io_context context;
    auto events = std::make_shared<RecordingEvents>();
    EXPECT_THROW((tcp_helper::TcpClient{context, events, {0, 4}}), std::invalid_argument);
    EXPECT_THROW((tcp_helper::TcpServer{context, events, {8, 3}}), std::invalid_argument);
}

TEST(TcpFailures, DoesNotKeepObserverAlive)
{
    boost::asio::io_context context;
    auto events = std::make_shared<RecordingEvents>();
    std::weak_ptr<RecordingEvents> observer = events;
    tcp_helper::TcpClient client{context, events};
    events.reset();
    EXPECT_TRUE(observer.expired());
}

// Catches dereferencing public client/state members after a callback destroys them.
TEST_F(TcpTest, CanDestroyClientFromConnectedCallback)
{
    std::unique_ptr<tcp_helper::TcpClient> owner;
    auto events = std::make_shared<DestroyOwnerEvents<tcp_helper::TcpClient>>(owner);
    std::weak_ptr<tcp_helper::RawTcpEvents> observer_lifetime = events;
    owner = std::make_unique<tcp_helper::TcpClient>(context, events);
    owner->connect("127.0.0.1", server.port());
    ASSERT_TRUE(until(
        [this, &owner, &events]
        {
            return !owner && events->disconnects.size() == 1 && server_events->disconnects.size() == 1;
        }));
    ASSERT_EQ(events->connections.size(), 1);
    std::weak_ptr<tcp_helper::TcpConnection> connection_lifetime = events->connections.front();
    events->connections.clear();
    context.restart();
    context.run_for(10ms);
    EXPECT_TRUE(connection_lifetime.expired());
    events.reset();
    EXPECT_TRUE(observer_lifetime.expired());
}

// Catches starting another accept/read after a callback destroys the public server.
TEST_F(TcpTest, CanDestroyServerFromConnectedCallback)
{
    std::unique_ptr<tcp_helper::TcpServer> owner;
    auto events = std::make_shared<DestroyOwnerEvents<tcp_helper::TcpServer>>(owner);
    std::weak_ptr<tcp_helper::RawTcpEvents> observer_lifetime = events;
    owner = std::make_unique<tcp_helper::TcpServer>(context, events);
    ASSERT_FALSE(owner->start("127.0.0.1", 0));
    client.connect("127.0.0.1", owner->port());
    ASSERT_TRUE(until(
        [this, &owner, &events]
        {
            return !owner && events->disconnects.size() == 1 && client_events->disconnects.size() == 1;
        }));
    ASSERT_EQ(events->connections.size(), 1);
    std::weak_ptr<tcp_helper::TcpConnection> connection_lifetime = events->connections.front();
    events->connections.clear();
    context.restart();
    context.run_for(10ms);
    EXPECT_TRUE(connection_lifetime.expired());
    events.reset();
    EXPECT_TRUE(observer_lifetime.expired());
}

} // namespace
