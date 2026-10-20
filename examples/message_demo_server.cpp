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
        connection_.online_ = true;
        connection_.client_ = std::move(client);
        connection_.data_ = TcpDataCollection();
    }
    void on_heartbeat(tcp_helper::MessageSenderPtr sender, const tcp_helper::Heartbeat& message) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
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

int main(int argc, char* argv[])
{
    try
    {
        if (argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--once"))
        {
            std::cerr << "Usage: tcp_message_demo_server [port=10101] [--once]\n";
            return 2;
        }
        const auto server_port = argc > 1 ? parse_port(argv[1], true) : std::uint16_t{10101};
        const bool once = argc == 3;
        spdlog::set_pattern("[%H:%M:%S.%e] [thread %t] [%l] %v");

        // The TCP library uses one event thread; the main thread runs application logic.
        AsioContext context(1);
        auto server_events_ = std::make_shared<ServerEventHandler>();
        tcp_helper::TcpServer server_(*context.context(), server_events_);
        TcpSession session(context, server_);
        const std::string server_address = "127.0.0.1";
        const auto result =
            context.invoke(std::bind(&tcp_helper::TcpServer::start, &server_, server_address, server_port));
        if (result)
        {
            spdlog::error("Failed to start server: {}", result.message());
            return EXIT_FAILURE;
        }
        const auto listening_port = context.invoke(std::bind(&tcp_helper::TcpServer::port, &server_));
        std::cout << "LISTENING " << listening_port << std::endl;
        spdlog::info("Main: TCP server started on {}:{}", server_address, listening_port);

        auto last_heartbeat = std::chrono::steady_clock::time_point();
        tcp_helper::SequenceNumber sequence = 0;
        while (!context.stop_requested())
        {
            const auto current = std::chrono::steady_clock::now();
            auto tcp_ = server_events_->fetch();
            if (tcp_.error_)
            {
                return EXIT_FAILURE;
            }
            for (const auto& heartbeat : tcp_.data_.m_heartbeats)
            {
                spdlog::info("Main: server received Heartbeat: sequence={}, login={}", heartbeat.m_sequence_number,
                             heartbeat.m_user_identifier.m_login_name);
            }
            for (const auto& cart : tcp_.data_.m_carts)
            {
                spdlog::info("Main: server received ShoppingCart: login={}, items={}",
                             cart.m_user_identifier.m_login_name, cart.m_items.size());
                context.send_cart(tcp_.client_, cart);
            }
            if (tcp_.online_ && tcp_.client_ && current >= last_heartbeat + std::chrono::seconds(1))
            {
                context.send_heartbeat(tcp_.client_, tcp_helper::Heartbeat{++sequence, {"server", 2}});
                last_heartbeat = current;
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
