#pragma once

#include <tcp_helper/tcp.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>
#include <atomic>
#include <csignal>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using SharedIoContext = std::shared_ptr<boost::asio::io_context>;

class AsioContext
{
public:
    explicit AsioContext(std::size_t thread_count)
        : io_context_(std::make_shared<boost::asio::io_context>()),
          work_guard_(boost::asio::make_work_guard(*io_context_)), signals_(*io_context_, SIGINT, SIGTERM)
    {
        if (thread_count == 0)
        {
            throw std::invalid_argument("AsioContext needs at least one thread");
        }
        signals_.async_wait(std::bind(&AsioContext::on_signal, this, std::placeholders::_1, std::placeholders::_2));
        threads_.reserve(thread_count);
        try
        {
            for (std::size_t index = 0; index < thread_count; ++index)
            {
                threads_.emplace_back(&AsioContext::run, this);
            }
        }
        catch (...)
        {
            io_context_->stop();
            join_threads();
            throw;
        }
    }
    ~AsioContext()
    {
        io_context_->stop();
        join_threads();
    }

    SharedIoContext context() const
    {
        return io_context_;
    }
    bool stop_requested() const
    {
        return stop_requested_.load();
    }
    template <typename F> void post(F&& function)
    {
        boost::asio::post(*io_context_, std::forward<F>(function));
    }
    // Use for setup and shutdown that must finish before the main thread continues.
    template <typename F> auto invoke(F&& function)
    {
        std::packaged_task<std::invoke_result_t<F>()> task(std::forward<F>(function));
        auto result = task.get_future();
        post(std::move(task));
        return result.get();
    }
    void send_heartbeat(tcp_helper::MessageSenderPtr sender, tcp_helper::Heartbeat message)
    {
        post(std::bind(&AsioContext::on_send_heartbeat, std::move(sender), std::move(message)));
    }
    void send_cart(tcp_helper::MessageSenderPtr sender, tcp_helper::ShoppingCart message)
    {
        post(std::bind(&AsioContext::on_send_cart, std::move(sender), std::move(message)));
    }
    // Stop the client/server first, then drain their cancellations before destroying them.
    void finish()
    {
        post(std::bind(&AsioContext::cancel_signals, this));
        work_guard_.reset();
        join_threads();
    }

private:
    void run()
    {
        spdlog::info("TCP: event thread started");
        io_context_->run();
    }
    void join_threads()
    {
        for (auto& thread : threads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }
    void on_signal(tcp_helper::ErrorCode error, int)
    {
        if (!error)
        {
            stop_requested_.store(true);
        }
    }
    void cancel_signals()
    {
        signals_.cancel();
    }
    static void on_send_heartbeat(const tcp_helper::MessageSenderPtr& sender, const tcp_helper::Heartbeat& message)
    {
        if (sender && sender->is_open() && !sender->send(message))
        {
            spdlog::error("Could not queue Heartbeat");
            sender->close();
        }
    }
    static void on_send_cart(const tcp_helper::MessageSenderPtr& sender, const tcp_helper::ShoppingCart& message)
    {
        if (sender && sender->is_open() && !sender->send(message))
        {
            spdlog::error("Could not queue ShoppingCart");
            sender->close();
        }
    }

    SharedIoContext io_context_;
    using work_guard_type = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    work_guard_type work_guard_;
    boost::asio::signal_set signals_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_requested_{false};
};

struct TcpDataCollection
{
    std::vector<tcp_helper::Heartbeat> m_heartbeats;
    std::vector<tcp_helper::ShoppingCart> m_carts;
};

struct TcpConnection
{
    TcpConnection() = default;
    // fetch() holds the source mutex while copying the connection snapshot.
    TcpConnection(const TcpConnection& value)
        : online_(value.online_), client_(value.client_), data_(value.data_),
          error_(value.error_)
    {
    }

    std::mutex mutex_;
    bool online_ = false;
    tcp_helper::MessageSenderPtr client_;
    TcpDataCollection data_;
    std::error_code error_;
};

// Ensures the TCP object is stopped and the context drained before either is destroyed.
template <typename Transport> class TcpSession
{
public:
    TcpSession(AsioContext& context, Transport& transport) : context_(context), transport_(transport)
    {
    }
    ~TcpSession()
    {
        context_.invoke(std::bind(&Transport::stop, &transport_));
        context_.finish();
    }
    TcpSession(const TcpSession&) = delete;
    TcpSession& operator=(const TcpSession&) = delete;

private:
    AsioContext& context_;
    Transport& transport_;
};
