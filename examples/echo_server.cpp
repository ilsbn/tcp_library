#include "arguments.hpp"
#include "echo_message_events.hpp"
#include <tcp_helper/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>
#include <csignal>
#include <functional>
#include <iostream>

namespace
{
class EchoServerApp;

class EchoEvents final : public EchoMessageEvents
{
public:
    explicit EchoEvents(EchoServerApp& app) : app_(app)
    {
    }
    void on_connected(tcp_helper::MessageSenderPtr) override
    {
        spdlog::info("Client connected");
    }
    void on_disconnected(tcp_helper::MessageSenderPtr, std::error_code reason) override;

private:
    EchoServerApp& app_;
};

class EchoServerApp final
{
public:
    explicit EchoServerApp(bool once)
        : events_(std::make_shared<EchoEvents>(*this)), server_(context_, events_), signals_(context_, SIGINT, SIGTERM),
          once_(once)
    {
    }

    int run(std::uint16_t port)
    {
        if (const auto error = server_.start("127.0.0.1", port))
        {
            spdlog::error("Listen failed: {}", error.message());
            return 1;
        }
        std::cout << "LISTENING " << server_.port() << std::endl;
        signals_.async_wait(std::bind(&EchoServerApp::on_signal, this, std::placeholders::_1, std::placeholders::_2));
        context_.run();
        return 0;
    }

    void on_disconnect()
    {
        if (once_)
        {
            stop();
        }
    }

private:
    void on_signal(tcp_helper::ErrorCode error, int)
    {
        if (!error)
        {
            stop();
        }
    }

    void stop()
    {
        signals_.cancel();
        server_.stop();
    }

    boost::asio::io_context context_;
    std::shared_ptr<EchoEvents> events_;
    tcp_helper::TcpServer server_;
    boost::asio::signal_set signals_;
    bool once_;
};

void EchoEvents::on_disconnected(tcp_helper::MessageSenderPtr, std::error_code reason)
{
    spdlog::info("Client disconnected: {}", reason.message());
    app_.on_disconnect();
}
} // namespace

int main(int argc, char* argv[])
{
    try
    {
        if (argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--once"))
        {
            std::cerr << "Usage: tcp_echo_server [port=7777] [--once]\n";
            return 2;
        }
        const auto port = argc > 1 ? parse_port(argv[1], true) : std::uint16_t{7777};
        EchoServerApp app{argc == 3};
        return app.run(port);
    }
    catch (const std::exception& error)
    {
        spdlog::error("{}", error.what());
        return 1;
    }
}
