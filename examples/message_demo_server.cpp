#include "arguments.hpp"
#include "message_demo_common.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace
{
// Application handler: no TCP, sockets, io_context, or byte encoding.
class ServerEventHandler final : public tcp_helper::MessageEvents
{
public:
    void on_connected(tcp_helper::MessageSenderPtr client) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        spdlog::info("TCP: client connected");
        connection_.client_ = std::move(client);
        connection_.data_ = TcpDataCollection();
    }
    void on_heartbeat(tcp_helper::MessageSenderPtr sender, const tcp_helper::Heartbeat& message) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        connection_.online_ = true;

        if (sender == connection_.client_)
        {
            spdlog::info("TCP: received Heartbeat");
            connection_.data_.m_heartbeats.push_back(message);
        }
    }
    void on_shopping_cart(tcp_helper::MessageSenderPtr sender, const tcp_helper::ShoppingCart& message) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        if (sender == connection_.client_)
        {
            spdlog::info("TCP: received ShoppingCart");
            connection_.data_.m_carts.push_back(message);
        }
    }
    void on_disconnected(tcp_helper::MessageSenderPtr sender, std::error_code) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        if (sender == connection_.client_)
        {
            spdlog::info("TCP: client disconnected");
            connection_.online_ = false;
            connection_.error_ = std::error_code();
            connection_.client_.reset();
        }
    }
    void on_error(std::string_view operation, std::error_code error) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        connection_.error_ = error;
        spdlog::error("{}: {}", operation, error.message());
    }

    TcpConnection fetch()
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        auto copy = connection_;
        connection_.data_ = TcpDataCollection();
        return copy;
    }

private:
    TcpConnection connection_;
};
} // namespace

int main()
{
    try
    {
        const std::string server_address = "127.0.0.1";
        const auto server_port = std::uint16_t{10101};

        spdlog::set_pattern("[%H:%M:%S.%e] [thread %t] [%l] %v");

        // The TCP library uses one event thread; the main thread runs application logic.
        AsioContext context(1);
        auto server_events_ = std::make_shared<ServerEventHandler>();
        tcp_helper::TcpServer server_(*context.context(), server_events_);
        TcpSession session(context, server_);

        if (const auto result = server_.start(server_address, server_port); result.failed())
        {
            spdlog::error("Failed to start server: {}", result.message());
            return EXIT_FAILURE;
        }

        spdlog::info("Main: TCP server started on {}:{}", server_address, server_.port());

        auto last_heartbeat = std::chrono::steady_clock::time_point();
        tcp_helper::SequenceNumber sequence = 0;
        while (!context.stop_requested())
        {
            const auto current = std::chrono::steady_clock::now();
            auto tcp_ = server_events_->fetch();

            if (tcp_.online_ && tcp_.client_)
            {
                for (const auto& heartbeat : tcp_.data_.m_heartbeats)
                {
                    spdlog::info("Main: server received Heartbeat: sequence={}, login={}", heartbeat.m_sequence_number, heartbeat.m_user_identifier.m_login_name);
                }
                for (const auto& cart : tcp_.data_.m_carts)
                {
                    spdlog::info("Main: server received ShoppingCart: login={}, items={}", cart.m_user_identifier.m_login_name, cart.m_items.size());
                    if (!tcp_.client_->send(cart))
                    {
                        spdlog::error("Could not queue ShoppingCart");
                        return EXIT_FAILURE;
                    }
                }
                if (current >= last_heartbeat + std::chrono::seconds(1))
                {
                    if (!tcp_.client_->send(tcp_helper::Heartbeat{++sequence, {"server", 2}}))
                    {
                        spdlog::error("Could not queue Heartbeat");
                        return EXIT_FAILURE;
                    }
                    last_heartbeat = current;
                }
            }

            // Put application logic here; callbacks collect messages while this thread works.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        spdlog::error("{}", error.what());
        return 2;
    }
}
