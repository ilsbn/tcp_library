#include "echo_message_events.hpp"
#include <tcp_helper/tcp.hpp>
#include <spdlog/spdlog.h>
#include <chrono>

namespace
{
class ProbeEvents final : public tcp_helper::TcpEvents
{
public:
    explicit ProbeEvents(boost::asio::io_context& context) : context_(context)
    {
    }
    void on_connected(tcp_helper::MessageSenderPtr sender) override
    {
        if (!sender->send(expected_))
        {
            context_.stop();
        }
    }
    void on_heartbeat(tcp_helper::MessageSenderPtr, const tcp_helper::Heartbeat& message) override
    {
        success = message == expected_;
        context_.stop();
    }
    void on_shopping_cart(tcp_helper::MessageSenderPtr, const tcp_helper::ShoppingCart&) override
    {
        context_.stop();
    }
    void on_disconnected(tcp_helper::MessageSenderPtr, std::error_code) override
    {
        context_.stop();
    }
    void on_error(std::string_view operation, std::error_code error) override
    {
        spdlog::error("Client {}: {}", operation, error.message());
        context_.stop();
    }
    bool success = false;

private:
    boost::asio::io_context& context_;
    const tcp_helper::Heartbeat expected_{42, {"smoke", 1234}};
};
} // namespace

int main()
{
    boost::asio::io_context context;
    auto echo = std::make_shared<EchoMessageEvents>();
    auto probe = std::make_shared<ProbeEvents>(context);
    tcp_helper::TcpServer server{context, echo};
    tcp_helper::TcpClient client{context, probe};
    if (const auto error = server.start("127.0.0.1", 0))
    {
        spdlog::error("Listen failed: {}", error.message());
        return 1;
    }
    client.connect("127.0.0.1", server.port());
    context.run_for(std::chrono::seconds(5));
    client.stop();
    server.stop();
    context.restart();
    context.poll();
    const bool success = probe->success && !echo->failed;
    spdlog::info("TCP smoke test {}", success ? "passed" : "failed");
    return success ? 0 : 1;
}
