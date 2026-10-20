#include <tcp_helper/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <vector>

using namespace std::chrono_literals;
using namespace tcp_helper;

namespace
{
class RecordingErrorLogger final : public IMessageErrorLogger
{
public:
    void log(const MessageErrorDetails& error) noexcept override
    {
        errors.push_back(error);
    }
    std::vector<MessageErrorDetails> errors;
};

class TypedRecordingEvents : public TcpEvents
{
public:
    void on_connected(MessageSenderPtr sender) override
    {
        senders.push_back(std::move(sender));
    }
    void on_heartbeat(MessageSenderPtr, const Heartbeat& message) override
    {
        messages.emplace_back(message);
    }
    void on_shopping_cart(MessageSenderPtr, const ShoppingCart& message) override
    {
        messages.emplace_back(message);
    }
    void on_disconnected(MessageSenderPtr, std::error_code reason) override
    {
        disconnects.push_back(reason);
    }
    void on_error(std::string_view, std::error_code error) override
    {
        errors.push_back(error);
    }

    std::vector<MessageSenderPtr> senders;
    std::vector<MessageBody> messages;
    std::vector<std::error_code> disconnects;
    std::vector<std::error_code> errors;
};

class TypedEchoEvents : public TypedRecordingEvents
{
public:
    void on_heartbeat(MessageSenderPtr sender, const Heartbeat& message) override
    {
        TypedRecordingEvents::on_heartbeat(sender, message);
        EXPECT_TRUE(sender->send(message));
    }
    void on_shopping_cart(MessageSenderPtr sender, const ShoppingCart& message) override
    {
        TypedRecordingEvents::on_shopping_cart(sender, message);
        EXPECT_TRUE(sender->send(message));
    }
};

template <class Owner> class DestroyTypedOwnerEvents final : public TypedRecordingEvents
{
public:
    explicit DestroyTypedOwnerEvents(std::unique_ptr<Owner>& owner) : owner_(owner)
    {
    }
    void on_connected(MessageSenderPtr sender) override
    {
        TypedRecordingEvents::on_connected(std::move(sender));
        owner_.reset();
    }

private:
    std::unique_ptr<Owner>& owner_;
};

class ClosingTypedEvents final : public TypedRecordingEvents
{
public:
    void on_heartbeat(MessageSenderPtr sender, const Heartbeat& message) override
    {
        TypedRecordingEvents::on_heartbeat(sender, message);
        sender->close();
    }
};

class DestroyOnDisconnectEvents final : public TypedRecordingEvents
{
public:
    explicit DestroyOnDisconnectEvents(std::unique_ptr<TcpServer>& owner) : owner_(owner)
    {
    }
    void on_disconnected(MessageSenderPtr sender, std::error_code reason) override
    {
        TypedRecordingEvents::on_disconnected(std::move(sender), reason);
        owner_.reset();
    }

private:
    std::unique_ptr<TcpServer>& owner_;
};

class RawPeerEvents final : public RawTcpEvents
{
public:
    void on_connected(ConnectionPtr connection) override
    {
        peer = std::move(connection);
    }
    void on_message(ConnectionPtr, DataBuffer) override
    {
    }
    void on_disconnected(ConnectionPtr, ErrorCode) override
    {
    }
    void on_error(std::string_view, ErrorCode) override
    {
    }
    ConnectionPtr peer;
};

class TypedTcpTest : public testing::Test
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
        const auto deadline = std::chrono::steady_clock::now() + 2s;
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
                return !client_events->senders.empty() && !server_events->senders.empty();
            });
    }
    bool raw_connect(boost::asio::ip::tcp::socket& raw)
    {
        raw.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
        return until(
            [this]
            {
                return !server_events->senders.empty();
            });
    }
    void write_packet(boost::asio::ip::tcp::socket& raw, const DataBuffer& encoded)
    {
        DataBuffer frame;
        frame.write_uint32(static_cast<std::uint32_t>(encoded.size()));
        frame.write_bytes(encoded.bytes());
        boost::asio::write(raw, boost::asio::buffer(frame.bytes().data(), frame.size()));
    }

    boost::asio::io_context context;
    std::shared_ptr<TypedEchoEvents> server_events = std::make_shared<TypedEchoEvents>();
    std::shared_ptr<TypedRecordingEvents> client_events = std::make_shared<TypedRecordingEvents>();
    std::shared_ptr<RecordingErrorLogger> error_logger = std::make_shared<RecordingErrorLogger>();
    TcpServer server{context, server_events, {1024 * 1024, 4 * 1024 * 1024, error_logger}};
    TcpClient client{context, client_events, {1024 * 1024, 4 * 1024 * 1024, error_logger}};
};

TEST_F(TypedTcpTest, ExchangesBothMessageTypesUsingOnlySenderInterface)
{
    ASSERT_TRUE(connect());
    const Heartbeat heartbeat{123, {std::string("a\0b", 3), 42}};
    const ShoppingCart cart{{"alice", 42}, {{10, 1.5F}, {20, -2.25F}}};
    const auto sender = client_events->senders.front();
    ASSERT_TRUE(sender->send(heartbeat));
    ASSERT_TRUE(sender->send(cart));
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->messages.size() == 2;
        }));
    const std::vector<MessageBody> expected{heartbeat, cart};
    EXPECT_EQ(server_events->messages, expected);
    EXPECT_EQ(client_events->messages, expected);
    EXPECT_TRUE(sender->is_open());
}

TEST_F(TypedTcpTest, ReceivedWireBytesStayInNetworkOrderAndDispatchCorrectValues)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    // Independently specified big-endian Heartbeat, including CRC d352ca3d.
    const std::array<std::uint8_t, 45> packet{
        0x54, 0x43, 0x50, 0x4d, 0x53, 0x47, 0x30, 0x31, 0, 0,    0, 0, 0, 0, 0, 0, 0, 0, 0,    0x15, 0,    0,   0,
        0,    0,    0,    0,    1,    0,    0,    0,    1, 0x41, 0, 0, 0, 0, 0, 0, 0, 2, 0xd3, 0x52, 0xca, 0x3d};
    write_packet(raw, DataBuffer{packet});
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 1;
        }));
    EXPECT_EQ(std::get<Heartbeat>(server_events->messages.front()), (Heartbeat{1, {"A", 2}}));
    ASSERT_TRUE(until(
        [&raw, &packet]
        {
            return raw.available() >= packet.size() + 4;
        }));
    std::array<std::uint8_t, 49> received{};
    boost::asio::read(raw, boost::asio::buffer(received));
    const std::array<std::uint8_t, 4> expected_length{0, 0, 0, 45};
    EXPECT_TRUE(std::equal(expected_length.begin(), expected_length.end(), received.begin()));
    EXPECT_TRUE(std::equal(packet.begin(), packet.end(), received.begin() + 4));
}

TEST_F(TypedTcpTest, OutgoingShoppingCartUsesNetworkOrderIncludingFloatAndChecksum)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    ASSERT_TRUE(server_events->senders.front()->send(ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}}));
    const std::array<std::uint8_t, 69> expected{0,    0, 0, 65, 0x54, 0x43, 0x50, 0x4d, 0x53, 0x47, 0x30, 0x31, 0,   0,
                                                0,    1, 0, 0,  0,    0,    0,    0,    0,    0x29, 0,    0,    0,   1,
                                                0x41, 0, 0, 0,  0,    0,    0,    0,    2,    0,    0,    0,    2,   0,
                                                0,    0, 0, 0,  0,    0,    3,    0x3f, 0xc0, 0,    0,    0,    0,   0,
                                                0,    0, 0, 0,  4,    0x40, 0x10, 0,    0,    0xb1, 0x6b, 0x1c, 0x68};
    ASSERT_TRUE(until(
        [&raw, &expected]
        {
            return raw.available() >= expected.size();
        }));
    std::array<std::uint8_t, 69> received{};
    boost::asio::read(raw, boost::asio::buffer(received));
    EXPECT_EQ(received, expected);
}

TEST_F(TypedTcpTest, CorruptPacketIsLoggedAndNextMessageUsesSameConnection)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    const Heartbeat expected{1, {"alice", 42}};
    const auto valid = encode_message(expected);
    auto corrupt = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    corrupt[message_header_size] ^= 1;
    write_packet(raw, DataBuffer{std::move(corrupt)});
    write_packet(raw, valid);
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 1;
        }));
    EXPECT_EQ(std::get<Heartbeat>(server_events->messages.front()), expected);
    ASSERT_EQ(error_logger->errors.size(), 1);
    EXPECT_EQ(error_logger->errors.front().code, MessageErrorCode::checksum_mismatch);
    EXPECT_NE(error_logger->errors.front().connection_id, 0);
    EXPECT_EQ(error_logger->errors.front().stream_offset, 4);
    EXPECT_TRUE(server_events->disconnects.empty());
    EXPECT_TRUE(server_events->senders.front()->is_open());
}

TEST_F(TypedTcpTest, MalformedPayloadIsLoggedAndNextMessageUsesSameConnection)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    DataBuffer packet;
    packet.write_uint64(message_magic_number);
    packet.write_uint32(static_cast<std::uint32_t>(MessageType::ShoppingCart));
    packet.write_uint64(0);
    packet.write_uint32(compute_crc32(packet.bytes()));
    write_packet(raw, packet);
    const ShoppingCart expected{{"alice", 42}, {{1, 2.5F}}};
    write_packet(raw, encode_message(expected));
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 1;
        }));
    EXPECT_EQ(std::get<ShoppingCart>(server_events->messages.front()), expected);
    ASSERT_EQ(error_logger->errors.size(), 1);
    EXPECT_EQ(error_logger->errors.front().code, MessageErrorCode::truncated_field);
    EXPECT_EQ(error_logger->errors.front().byte_offset, message_header_size);
    EXPECT_TRUE(server_events->disconnects.empty());
    EXPECT_TRUE(server_events->senders.front()->is_open());
}

TEST_F(TypedTcpTest, TypedLimitsAndCloseAreExposedThroughInterface)
{
    auto events = std::make_shared<TypedRecordingEvents>();
    const Heartbeat heartbeat{1, {"A", 2}}; // Encoded 45 bytes, framed 49 bytes.
    TcpClient limited{context, events, {45, 49}};
    limited.connect("127.0.0.1", server.port());
    ASSERT_TRUE(until(
        [&events]
        {
            return !events->senders.empty();
        }));
    const auto sender = events->senders.front();
    EXPECT_FALSE(sender->send(Heartbeat{1, {"too long", 2}}));
    ASSERT_TRUE(sender->send(heartbeat));
    EXPECT_FALSE(sender->send(heartbeat));
    ASSERT_TRUE(until(
        [&events]
        {
            return events->messages.size() == 1;
        }));
    EXPECT_TRUE(sender->send(heartbeat));
    sender->close();
    sender->close();
    EXPECT_FALSE(sender->is_open());
    EXPECT_FALSE(sender->send(heartbeat));
    EXPECT_FALSE(sender->send(ShoppingCart{}));
    EXPECT_EQ(events->disconnects.size(), 1);
    limited.stop();
}

TEST(TypedTcpLifetime, DoesNotKeepApplicationObserverAlive)
{
    boost::asio::io_context context;
    auto events = std::make_shared<TypedRecordingEvents>();
    std::weak_ptr<MessageEvents> lifetime = events;
    TcpClient client{context, events};
    events.reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(TypedTcpTest, ClientLogsCorruptReplyAndReceivesNextReplyWithoutReconnect)
{
    auto peer_events = std::make_shared<RawPeerEvents>();
    TcpServer peer{context, peer_events};
    ASSERT_FALSE(peer.start("127.0.0.1", 0));
    client.connect("127.0.0.1", peer.port());
    ASSERT_TRUE(until(
        [&peer_events]
        {
            return peer_events->peer != nullptr;
        }));
    const ShoppingCart expected{{"alice", 42}, {{1, 2.5F}}};
    auto valid = encode_message(expected);
    auto corrupt = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    corrupt.back() ^= 1;
    ASSERT_TRUE(peer_events->peer->send(DataBuffer{std::move(corrupt)}));
    ASSERT_TRUE(peer_events->peer->send(valid));
    ASSERT_TRUE(until(
        [this]
        {
            return client_events->messages.size() == 1;
        }));
    EXPECT_EQ(std::get<ShoppingCart>(client_events->messages.front()), expected);
    ASSERT_EQ(error_logger->errors.size(), 1);
    EXPECT_EQ(error_logger->errors.front().code, MessageErrorCode::checksum_mismatch);
    EXPECT_TRUE(client_events->disconnects.empty());
    EXPECT_TRUE(client_events->senders.front()->is_open());
    peer.stop();
}

TEST_F(TypedTcpTest, CanCloseFromTypedCallbackWithoutDeliveringQueuedMessage)
{
    auto events = std::make_shared<ClosingTypedEvents>();
    TcpServer receiver{context, events};
    ASSERT_FALSE(receiver.start("127.0.0.1", 0));
    client.connect("127.0.0.1", receiver.port());
    ASSERT_TRUE(until(
        [this]
        {
            return !client_events->senders.empty();
        }));
    ASSERT_TRUE(client_events->senders.front()->send(Heartbeat{1, {"alice", 42}}));
    ASSERT_TRUE(client_events->senders.front()->send(ShoppingCart{}));
    ASSERT_TRUE(until(
        [this]
        {
            return !client_events->disconnects.empty();
        }));
    ASSERT_EQ(events->messages.size(), 1);
    EXPECT_TRUE(std::holds_alternative<Heartbeat>(events->messages.front()));
    EXPECT_EQ(events->disconnects.size(), 1);
    receiver.stop();
}

TEST_F(TypedTcpTest, CanDestroyClientFromTypedConnectedCallback)
{
    std::unique_ptr<TcpClient> owner;
    auto events = std::make_shared<DestroyTypedOwnerEvents<TcpClient>>(owner);
    owner = std::make_unique<TcpClient>(context, events);
    owner->connect("127.0.0.1", server.port());
    ASSERT_TRUE(until(
        [&owner, &events]
        {
            return !owner && events->disconnects.size() == 1;
        }));
    ASSERT_EQ(events->senders.size(), 1);
    std::weak_ptr<IMessageSender> lifetime = events->senders.front();
    events->senders.clear();
    context.restart();
    context.run_for(25ms);
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(TypedTcpTest, CanDestroyServerFromTypedConnectedCallback)
{
    std::unique_ptr<TcpServer> owner;
    auto events = std::make_shared<DestroyTypedOwnerEvents<TcpServer>>(owner);
    owner = std::make_unique<TcpServer>(context, events);
    ASSERT_FALSE(owner->start("127.0.0.1", 0));
    client.connect("127.0.0.1", owner->port());
    ASSERT_TRUE(until(
        [&owner, &events]
        {
            return !owner && events->disconnects.size() == 1;
        }));
    ASSERT_EQ(events->senders.size(), 1);
    std::weak_ptr<IMessageSender> lifetime = events->senders.front();
    events->senders.clear();
    context.restart();
    context.run_for(25ms);
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(TypedTcpTest, ExpiredApplicationObserverIsSafeDuringReceiveAndShutdown)
{
    auto events = std::make_shared<TypedRecordingEvents>();
    std::weak_ptr<MessageEvents> lifetime = events;
    TcpClient unobserved{context, events};
    unobserved.connect("127.0.0.1", server.port());
    ASSERT_TRUE(until(
        [&events]
        {
            return !events->senders.empty();
        }));
    ASSERT_TRUE(events->senders.front()->send(Heartbeat{1, {"alice", 42}}));
    events.reset();
    EXPECT_TRUE(lifetime.expired());
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 1;
        }));
    unobserved.stop();
}

TEST_F(TypedTcpTest, DestroyingServerDuringFirstDisconnectStillNotifiesEveryConnection)
{
    std::unique_ptr<TcpServer> owner;
    auto events = std::make_shared<DestroyOnDisconnectEvents>(owner);
    owner = std::make_unique<TcpServer>(context, events);
    ASSERT_FALSE(owner->start("127.0.0.1", 0));
    auto second_events = std::make_shared<TypedRecordingEvents>();
    TcpClient second{context, second_events};
    client.connect("127.0.0.1", owner->port());
    second.connect("127.0.0.1", owner->port());
    ASSERT_TRUE(until(
        [&events]
        {
            return events->senders.size() == 2;
        }));
    owner->stop();
    ASSERT_TRUE(until(
        [&owner]
        {
            return !owner;
        }));
    EXPECT_EQ(events->disconnects.size(), 2);
    EXPECT_FALSE(events->senders[0]->is_open());
    EXPECT_FALSE(events->senders[1]->is_open());
    ASSERT_TRUE(until(
        [this, &second_events]
        {
            return client_events->disconnects.size() == 1 && second_events->disconnects.size() == 1;
        }));
    second.stop();
}

TEST(TypedTcpLifetime, RejectsNullTypedObservers)
{
    boost::asio::io_context context;
    std::shared_ptr<TcpEvents> empty;
    EXPECT_THROW((TcpClient{context, empty}), std::invalid_argument);
    EXPECT_THROW((TcpServer{context, empty}), std::invalid_argument);
}

TEST_F(TypedTcpTest, GarbageAndEveryOuterLengthBitErrorRecoverOnSameConnection)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    const auto sender = server_events->senders.front();
    DataBuffer stream{std::vector<std::uint8_t>(13, 0xff)};
    for (unsigned index = 0; index <= 32; ++index)
    {
        const auto packet = encode_message(Heartbeat{index, {"A", 2}});
        auto length = static_cast<std::uint32_t>(packet.size());
        if (index != 0)
        {
            length ^= std::uint32_t{1} << (index - 1);
        }
        stream.write_uint32(length);
        stream.write_bytes(packet.bytes());
    }
    boost::asio::write(raw, boost::asio::buffer(stream.bytes().data(), stream.size()));
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 33;
        }));
    for (std::size_t index = 0; index < server_events->messages.size(); ++index)
    {
        EXPECT_EQ(std::get<Heartbeat>(server_events->messages[index]).m_sequence_number, index);
    }
    EXPECT_EQ(error_logger->errors.size(), 33);
    EXPECT_TRUE(server_events->disconnects.empty());
    EXPECT_TRUE(sender->is_open());
    ASSERT_EQ(server_events->senders.size(), 1);
    EXPECT_EQ(server_events->senders.front(), sender);
}

TEST_F(TypedTcpTest, EveryMessageBitErrorIsLoggedAndFollowedByValidDelivery)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    const std::array<DataBuffer, 2> packets{encode_message(Heartbeat{1, {"A", 2}}),
                                            encode_message(ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}})};
    DataBuffer batch;
    std::size_t expected_messages = 0;
    for (const auto& packet : packets)
    {
        for (std::size_t offset = 0; offset < packet.size(); ++offset)
        {
            for (unsigned bit = 0; bit < 8; ++bit)
            {
                auto bytes = std::vector<std::uint8_t>(packet.bytes().begin(), packet.bytes().end());
                bytes[offset] ^= static_cast<std::uint8_t>(1u << bit);
                batch.write_uint32(static_cast<std::uint32_t>(bytes.size()));
                batch.write_bytes(bytes);
                const auto valid = encode_message(Heartbeat{++expected_messages, {"valid", 42}});
                batch.write_uint32(static_cast<std::uint32_t>(valid.size()));
                batch.write_bytes(valid.bytes());
                if (expected_messages % 16 == 0)
                {
                    boost::asio::write(raw, boost::asio::buffer(batch.bytes().data(), batch.size()));
                    ASSERT_TRUE(until(
                        [this, expected_messages]
                        {
                            return server_events->messages.size() == expected_messages;
                        }));
                    batch.clear();
                }
            }
        }
    }
    if (!batch.empty())
    {
        boost::asio::write(raw, boost::asio::buffer(batch.bytes().data(), batch.size()));
    }
    ASSERT_TRUE(until(
        [this, expected_messages]
        {
            return server_events->messages.size() == expected_messages;
        }));
    EXPECT_EQ(expected_messages, (45 + 65) * 8u);
    for (std::size_t index = 0; index < expected_messages; ++index)
    {
        EXPECT_EQ(std::get<Heartbeat>(server_events->messages[index]).m_sequence_number, index + 1);
    }
    EXPECT_GE(error_logger->errors.size(), expected_messages);
    EXPECT_TRUE(server_events->disconnects.empty());
    EXPECT_TRUE(server_events->senders.front()->is_open());
}

TEST_F(TypedTcpTest, PeerClosingMidFrameLogsTruncationBeforeDisconnect)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    const auto packet = encode_message(Heartbeat{1, {"A", 2}});
    DataBuffer frame;
    frame.write_uint32(static_cast<std::uint32_t>(packet.size()));
    frame.write_bytes(packet.bytes().first(13));
    boost::asio::write(raw, boost::asio::buffer(frame.bytes().data(), frame.size()));
    raw.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
    raw.close();
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->disconnects.size() == 1;
        }));
    ASSERT_EQ(error_logger->errors.size(), 1);
    EXPECT_EQ(error_logger->errors.front().code, MessageErrorCode::truncated_frame);
    EXPECT_EQ(error_logger->errors.front().expected, packet.size() + 4);
    EXPECT_EQ(error_logger->errors.front().actual, frame.size());
    EXPECT_TRUE(server_events->messages.empty());
}

TEST_F(TypedTcpTest, UnsupportedTypeRecoversAtNextMagicWithoutWaitingForAdvertisedBody)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    DataBuffer partial;
    partial.write_uint32(900024);
    partial.write_uint64(message_magic_number);
    partial.write_uint32(99);
    partial.write_uint64(900000);
    boost::asio::write(raw, boost::asio::buffer(partial.bytes().data(), partial.size()));
    ASSERT_TRUE(until(
        [this]
        {
            return !error_logger->errors.empty();
        }));
    ASSERT_EQ(error_logger->errors.size(), 1);
    EXPECT_EQ(error_logger->errors.front().code, MessageErrorCode::unknown_message_type);
    const Heartbeat expected{7, {"A", 2}};
    write_packet(raw, encode_message(expected));
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->messages.size() == 1;
        }));
    EXPECT_EQ(std::get<Heartbeat>(server_events->messages.front()), expected);
    EXPECT_TRUE(server_events->disconnects.empty());
    EXPECT_TRUE(server_events->senders.front()->is_open());
}

TEST_F(TypedTcpTest, PeerCloseRecoversValidMessageBehindLargeTruncatedFrame)
{
    boost::asio::ip::tcp::socket raw{context};
    ASSERT_TRUE(raw_connect(raw));
    const auto large = encode_message(ShoppingCart{{std::string(1000, 'x'), 42}, {}});
    DataBuffer stream;
    stream.write_uint32(static_cast<std::uint32_t>(large.size()));
    stream.write_bytes(large.bytes().first(message_header_size));
    const Heartbeat expected{7, {"A", 2}};
    const auto small = encode_message(expected);
    stream.write_uint32(static_cast<std::uint32_t>(small.size()));
    stream.write_bytes(small.bytes());
    boost::asio::write(raw, boost::asio::buffer(stream.bytes().data(), stream.size()));
    raw.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
    ASSERT_TRUE(until(
        [this]
        {
            return server_events->disconnects.size() == 1;
        }));
    ASSERT_EQ(server_events->messages.size(), 1);
    EXPECT_EQ(std::get<Heartbeat>(server_events->messages.front()), expected);
    ASSERT_FALSE(error_logger->errors.empty());
    EXPECT_EQ(error_logger->errors.front().code, MessageErrorCode::truncated_frame);
}
} // namespace
