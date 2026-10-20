#include "arguments.hpp"
#include <tcp_helper/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <functional>
#include <iostream>

namespace
{
class EchoClientApp;

class ClientEvents final : public tcp_helper::TcpEvents
{
public:
    explicit ClientEvents(EchoClientApp& app) : app_(app)
    {
    }
    void on_connected(tcp_helper::MessageSenderPtr connection) override;
    void on_heartbeat(tcp_helper::MessageSenderPtr, const tcp_helper::Heartbeat& message) override;
    void on_shopping_cart(tcp_helper::MessageSenderPtr, const tcp_helper::ShoppingCart&) override;
    void on_disconnected(tcp_helper::MessageSenderPtr, std::error_code reason) override;
    void on_error(std::string_view operation, std::error_code error) override;

private:
    EchoClientApp& app_;
};

class EchoClientApp final
{
public:
    explicit EchoClientApp(std::string message)
        : events_(std::make_shared<ClientEvents>(*this)), client_(context_, events_), timer_(context_),
          message_(std::move(message))
    {
    }

    int run(std::string host, std::uint16_t port)
    {
        timer_.expires_after(std::chrono::seconds(5));
        timer_.async_wait(std::bind(&EchoClientApp::on_timeout, this, std::placeholders::_1));
        client_.connect(std::move(host), port);
        context_.run();
        return success_ ? 0 : 1;
    }

    void send(tcp_helper::MessageSenderPtr connection)
    {
        const tcp_helper::Heartbeat heartbeat{1, {message_, 0}};
        if (!connection->send(heartbeat))
        {
            finish(false);
        }
    }

    void received(const tcp_helper::Heartbeat& heartbeat)
    {
        const auto& text = heartbeat.m_user_identifier.m_login_name;
        if (heartbeat.m_sequence_number == 1 && heartbeat.m_user_identifier.m_user_id == 0 && text == message_)
        {
            spdlog::info("Echo received: {}", text);
            finish(true);
        }
        else
        {
            spdlog::error("Echo does not match the sent message");
            finish(false);
        }
    }

    void finish(bool success)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        success_ = success;
        timer_.cancel();
        client_.stop();
    }

private:
    void on_timeout(tcp_helper::ErrorCode error)
    {
        if (!error)
        {
            spdlog::error("Timed out waiting for an echo");
            finish(false);
        }
    }

    boost::asio::io_context context_;
    std::shared_ptr<ClientEvents> events_;
    tcp_helper::TcpClient client_;
    boost::asio::steady_timer timer_;
    std::string message_;
    bool finished_ = false;
    bool success_ = false;
};

void ClientEvents::on_connected(tcp_helper::MessageSenderPtr connection)
{
    app_.send(std::move(connection));
}
void ClientEvents::on_heartbeat(tcp_helper::MessageSenderPtr, const tcp_helper::Heartbeat& message)
{
    app_.received(message);
}
void ClientEvents::on_shopping_cart(tcp_helper::MessageSenderPtr, const tcp_helper::ShoppingCart&)
{
    app_.finish(false);
}
void ClientEvents::on_disconnected(tcp_helper::MessageSenderPtr, std::error_code reason)
{
    spdlog::debug("Disconnected: {}", reason.message());
    app_.finish(false);
}
void ClientEvents::on_error(std::string_view operation, std::error_code error)
{
    spdlog::error("{}: {}", operation, error.message());
    app_.finish(false);
}
} // namespace

int main(int argc, char* argv[])
{
    try
    {
        if (argc > 4)
        {
            std::cerr << "Usage: tcp_echo_client [host=127.0.0.1] [port=7777] [message]\n";
            return 2;
        }
        const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
        const auto port = argc > 2 ? parse_port(argv[2]) : std::uint16_t{7777};
        EchoClientApp app{argc > 3 ? argv[3] : "Hello TCP"};
        return app.run(host, port);
    }
    catch (const std::exception& error)
    {
        spdlog::error("{}", error.what());
        return 1;
    }
}
