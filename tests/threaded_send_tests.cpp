#include <tcp_helper/tcp.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace tcp_helper;

namespace
{
class ThreadedEvents final : public MessageEvents
{
public:
    void on_connected(MessageSenderPtr sender) override
    {
        auto guard = std::lock_guard(mutex_);
        sender_ = std::move(sender);
        changed_.notify_all();
    }
    void on_heartbeat(MessageSenderPtr, const Heartbeat&) override
    {
    }
    void on_shopping_cart(MessageSenderPtr, const ShoppingCart&) override
    {
    }
    void on_disconnected(MessageSenderPtr, std::error_code) override
    {
        auto guard = std::lock_guard(mutex_);
        disconnected_ = true;
        disconnect_thread_ = std::this_thread::get_id();
        changed_.notify_all();
    }
    void on_error(std::string_view, std::error_code) override
    {
        auto guard = std::lock_guard(mutex_);
        failed_ = true;
        changed_.notify_all();
    }
    MessageSenderPtr wait_for_sender()
    {
        auto guard = std::unique_lock(mutex_);
        changed_.wait_for(guard, 3s,
                          [this]
                          {
                              return sender_ || failed_;
                          });
        return sender_;
    }
    bool wait_for_disconnect()
    {
        auto guard = std::unique_lock(mutex_);
        return changed_.wait_for(guard, 3s,
                                 [this]
                                 {
                                     return disconnected_;
                                 });
    }
    bool disconnected()
    {
        auto guard = std::lock_guard(mutex_);
        return disconnected_;
    }
    std::thread::id disconnect_thread()
    {
        auto guard = std::lock_guard(mutex_);
        return disconnect_thread_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    MessageSenderPtr sender_;
    bool disconnected_ = false;
    bool failed_ = false;
    std::thread::id disconnect_thread_;
};

// A paused event thread makes sends waiting for the executor observable without races.
class PausedTcpThread
{
public:
    explicit PausedTcpThread(boost::asio::io_context& context)
    {
        auto entered = std::make_shared<std::promise<void>>();
        entered_ = entered->get_future();
        const auto resumed = resume_.get_future().share();
        boost::asio::post(context,
                          [entered, resumed]
                          {
                              entered->set_value();
                              resumed.wait_for(3s);
                          });
    }
    ~PausedTcpThread()
    {
        resume_.set_value();
    }
    bool ready()
    {
        return entered_.wait_for(3s) == std::future_status::ready;
    }

private:
    std::promise<void> resume_;
    std::future<void> entered_;
};

DataBuffer frame(const DataBuffer& encoded)
{
    DataBuffer result;
    result.write_uint32(static_cast<std::uint32_t>(encoded.size()));
    result.write_bytes(encoded.bytes());
    return result;
}

class ThreadedSendTest : public testing::Test
{
protected:
    MessageSenderPtr connect(TcpOptions options = {})
    {
        client_ = std::make_unique<TcpClient>(context_, events_, options);
        worker_ = std::thread(
            [this]
            {
                context_.run();
            });
        client_->connect("127.0.0.1", acceptor_.local_endpoint().port());
        acceptor_.accept(peer_);
        return events_->wait_for_sender();
    }
    MessageSenderPtr connect_to_server()
    {
        server_ = std::make_unique<TcpServer>(context_, events_);
        if (server_->start("127.0.0.1", 0))
        {
            return {};
        }
        worker_ = std::thread(
            [this]
            {
                context_.run();
            });
        peer_.connect({boost::asio::ip::make_address("127.0.0.1"), server_->port()});
        return events_->wait_for_sender();
    }
    void TearDown() override
    {
        if (worker_.joinable())
        {
            if (client_)
            {
                client_->stop();
            }
            if (server_)
            {
                server_->stop();
            }
            work_.reset();
            worker_.join();
        }
    }
    std::vector<std::uint8_t> read_bytes(std::size_t count, std::chrono::milliseconds timeout = 3s)
    {
        std::vector<std::uint8_t> result;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (result.size() < count && std::chrono::steady_clock::now() < deadline)
        {
            const auto available = peer_.available();
            if (available == 0)
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            const auto offset = result.size();
            result.resize(offset + available);
            const auto received = peer_.read_some(boost::asio::buffer(result.data() + offset, available));
            result.resize(offset + received);
        }
        return result;
    }

    boost::asio::io_context peer_context_;
    boost::asio::ip::tcp::acceptor acceptor_{peer_context_, {boost::asio::ip::tcp::v4(), 0}};
    boost::asio::ip::tcp::socket peer_{peer_context_};
    boost::asio::io_context context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_{
        boost::asio::make_work_guard(context_)};
    std::shared_ptr<ThreadedEvents> events_ = std::make_shared<ThreadedEvents>();
    std::unique_ptr<TcpClient> client_;
    std::unique_ptr<TcpServer> server_;
    std::thread worker_;
};

TEST_F(ThreadedSendTest, MainThreadSendLeavesSocketWorkOnTcpThread)
{
    const auto sender = connect();
    ASSERT_TRUE(sender);
    const Heartbeat heartbeat{1, {"alice", 42}};
    const auto expected = frame(encode_message(heartbeat));
    std::vector<std::uint8_t> received;
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        ASSERT_TRUE(sender->send(heartbeat));
        received = read_bytes(expected.size(), 50ms);
        EXPECT_TRUE(received.empty()) << "send() touched the socket while the TCP thread was paused";
    }
    const auto remaining = read_bytes(expected.size() - received.size());
    received.insert(received.end(), remaining.begin(), remaining.end());
    EXPECT_EQ(DataBuffer{std::move(received)}, expected);
}

TEST_F(ThreadedSendTest, MainThreadSendsOwnTheirBytesAndPreserveOrderWhileWritesComplete)
{
    const auto sender = connect();
    ASSERT_TRUE(sender);
    DataBuffer expected;
    for (SequenceNumber sequence = 0; sequence < 512; ++sequence)
    {
        Heartbeat heartbeat{sequence, {"alice", 42}};
        ShoppingCart cart{{"alice", 42}, {{sequence, 1.5F}}};
        expected.write_bytes(frame(encode_message(heartbeat)).bytes());
        expected.write_bytes(frame(encode_message(cart)).bytes());
        ASSERT_TRUE(sender->send(heartbeat));
        ASSERT_TRUE(sender->send(cart));
        heartbeat.m_user_identifier.m_login_name = "changed";
        cart.m_items.clear();
    }
    EXPECT_EQ(DataBuffer{read_bytes(expected.size())}, expected);
}

TEST_F(ThreadedSendTest, QueueLimitIncludesSendsWaitingForTcpThreadAndRecoversAfterWrite)
{
    const auto sender = connect({45, 49});
    ASSERT_TRUE(sender);
    const Heartbeat heartbeat{1, {"A", 2}}; // Encoded 45 bytes, framed 49 bytes.
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        EXPECT_FALSE(sender->send(Heartbeat{1, {"too long", 2}}));
        ASSERT_TRUE(sender->send(heartbeat));
        EXPECT_FALSE(sender->send(heartbeat));
    }
    const auto expected = frame(encode_message(heartbeat));
    EXPECT_EQ(DataBuffer{read_bytes(expected.size())}, expected);
    bool accepted = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!accepted && std::chrono::steady_clock::now() < deadline)
    {
        accepted = sender->send(heartbeat);
        if (!accepted)
        {
            std::this_thread::sleep_for(1ms);
        }
    }
    ASSERT_TRUE(accepted);
    EXPECT_EQ(DataBuffer{read_bytes(expected.size())}, expected);
}

TEST_F(ThreadedSendTest, MainThreadObservesRemoteCloseAndRejectsFurtherSends)
{
    const auto sender = connect();
    ASSERT_TRUE(sender);
    EXPECT_TRUE(sender->is_open());
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        for (SequenceNumber sequence = 0; sequence < 128; ++sequence)
        {
            ASSERT_TRUE(sender->send(Heartbeat{sequence, {"alice", 42}}));
        }
        peer_.close();
    }
    ASSERT_TRUE(events_->wait_for_disconnect());
    EXPECT_FALSE(sender->is_open());
    EXPECT_FALSE(sender->send(Heartbeat{1, {"alice", 42}}));
    EXPECT_FALSE(sender->send(ShoppingCart{}));
}

TEST_F(ThreadedSendTest, ShutdownDrainsPostedSendsAndLeavesRetainedSenderClosed)
{
    const auto sender = connect();
    ASSERT_TRUE(sender);
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        boost::asio::post(context_,
                          [this]
                          {
                              client_->stop();
                          });
        for (SequenceNumber sequence = 0; sequence < 128; ++sequence)
        {
            ASSERT_TRUE(sender->send(Heartbeat{sequence, {"alice", 42}}));
        }
    }
    ASSERT_TRUE(events_->wait_for_disconnect());
    EXPECT_FALSE(sender->is_open());
    EXPECT_FALSE(sender->send(Heartbeat{}));
}

TEST_F(ThreadedSendTest, MainThreadClientStopRunsDisconnectionOnTcpThread)
{
    const auto sender = connect();
    ASSERT_TRUE(sender);
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        client_->stop();
        client_->stop();
        EXPECT_FALSE(events_->disconnected());
    }
    ASSERT_TRUE(events_->wait_for_disconnect());
    EXPECT_EQ(events_->disconnect_thread(), worker_.get_id());
    EXPECT_FALSE(sender->is_open());
}

TEST_F(ThreadedSendTest, MainThreadServerStopRunsDisconnectionOnTcpThread)
{
    const auto sender = connect_to_server();
    ASSERT_TRUE(sender);
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        server_->stop();
        server_->stop();
        EXPECT_FALSE(events_->disconnected());
    }
    ASSERT_TRUE(events_->wait_for_disconnect());
    EXPECT_EQ(events_->disconnect_thread(), worker_.get_id());
    EXPECT_FALSE(sender->is_open());
}

TEST_F(ThreadedSendTest, QueuedConnectRejectsRepeatedRequestsAndCanBeStoppedBeforeInitiation)
{
    client_ = std::make_unique<TcpClient>(context_, events_);
    worker_ = std::thread(
        [this]
        {
            context_.run();
        });
    {
        PausedTcpThread pause(context_);
        ASSERT_TRUE(pause.ready());
        client_->connect("127.0.0.1", acceptor_.local_endpoint().port());
        EXPECT_THROW(client_->connect("127.0.0.1", acceptor_.local_endpoint().port()), std::logic_error);
        client_->stop();
        EXPECT_THROW(client_->connect("127.0.0.1", acceptor_.local_endpoint().port()), std::logic_error);
    }
    // Cancelling a connect before it starts must not establish a peer connection.
    auto drained = std::make_shared<std::promise<void>>();
    auto result = drained->get_future();
    boost::asio::post(context_,
                      [drained]
                      {
                          drained->set_value();
                      });
    ASSERT_EQ(result.wait_for(3s), std::future_status::ready);
    acceptor_.non_blocking(true);
    ErrorCode error;
    acceptor_.accept(peer_, error);
    EXPECT_EQ(error, boost::asio::error::would_block);
}
} // namespace
