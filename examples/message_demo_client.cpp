#include "arguments.hpp"
#include "message_demo_common.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace
{
// Callbacks collect data; the main loop fetches and processes it.
class ClientEventHandler final : public tcp_helper::MessageEvents
{
public:
    void on_connected(tcp_helper::MessageSenderPtr server) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        spdlog::info("TCP: connected to server");
        connection_.online_ = true;
        connection_.client_ = std::move(server);
    }
    void on_heartbeat(tcp_helper::MessageSenderPtr, const tcp_helper::Heartbeat& message) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        spdlog::info("TCP: received Heartbeat");
        connection_.data_.m_heartbeats.push_back(message);
    }
    void on_shopping_cart(tcp_helper::MessageSenderPtr, const tcp_helper::ShoppingCart& message) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        spdlog::info("TCP: received ShoppingCart");
        connection_.data_.m_carts.push_back(message);
    }
    void on_disconnected(tcp_helper::MessageSenderPtr, std::error_code) override
    {
        auto guard = std::lock_guard<std::mutex>(connection_.mutex_);
        spdlog::info("TCP: server disconnected");
        connection_.online_ = false;
        connection_.error_ = std::error_code();
        connection_.client_.reset();
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
        connection_.error_ = std::error_code();
        return copy;
    }

private:
    TcpConnection connection_;
};
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc > 4 || (argc == 4 && std::string_view(argv[3]) != "--once"))
        {
            std::cerr << "Usage: tcp_message_demo_client [host=127.0.0.1] [port=10101] [--once]\n";
            return 2;
        }
        const std::string server_address = argc > 1 ? argv[1] : "127.0.0.1";
        const auto server_port = argc > 2 ? parse_port(argv[2]) : std::uint16_t{10101};
        const bool once = argc == 4;
        spdlog::set_pattern("[%H:%M:%S.%e] [thread %t] [%l] %v");

        AsioContext context(1);
        auto client_events_ = std::make_shared<ClientEventHandler>();
        tcp_helper::TcpClient client_(*context.context(), client_events_);
        TcpSession session(context, client_);
        context.invoke(std::bind(&tcp_helper::TcpClient::connect, &client_, server_address, server_port));
        spdlog::info("Main: connecting to {}:{}", server_address, server_port);

        const tcp_helper::ShoppingCart cart{{"alice", 42}, {{10001, 12.50F}, {10002, 3.25F}}};
        auto last_heartbeat = std::chrono::steady_clock::time_point();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        tcp_helper::SequenceNumber sequence = 0;
        bool cart_sent = false;
        bool received_heartbeat = false;
        bool received_cart = false;
        while (!context.stop_requested())
        {
            const auto current = std::chrono::steady_clock::now();
            auto tcp_ = client_events_->fetch();
            if (tcp_.error_)
            {
                return EXIT_FAILURE;
            }
            for (const auto& heartbeat : tcp_.data_.m_heartbeats)
            {
                spdlog::info("Main: client received Heartbeat: sequence={}, login={}", heartbeat.m_sequence_number,
                             heartbeat.m_user_identifier.m_login_name);
                received_heartbeat = true;
            }
            for (const auto& reply : tcp_.data_.m_carts)
            {
                spdlog::info("Main: client received ShoppingCart: login={}, items={}",
                             reply.m_user_identifier.m_login_name, reply.m_items.size());
                if (reply != cart)
                {
                    spdlog::error("ShoppingCart reply does not match the sent values");
                    return EXIT_FAILURE;
                }
                received_cart = true;
            }
            if (once && received_heartbeat && received_cart)
            {
                return EXIT_SUCCESS;
            }
            if (once && current >= deadline)
            {
                spdlog::error("Timed out waiting for the message exchange");
                return EXIT_FAILURE;
            }
            if (tcp_.online_ && tcp_.client_)
            {
                if (!cart_sent)
                {
                    context.send_cart(tcp_.client_, cart);
                    cart_sent = true;
                }
                if (current >= last_heartbeat + std::chrono::seconds(1))
                {
                    context.send_heartbeat(tcp_.client_, tcp_helper::Heartbeat{++sequence, {"alice", 42}});
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
