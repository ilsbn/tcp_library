#include "connection_state.hpp"
#include "typed_events.hpp"
#include <spdlog/spdlog.h>
#include <functional>
#include <map>

namespace tcp_helper::detail
{

class ServerState final : public ConnectionOwner, public std::enable_shared_from_this<ServerState>
{
public:
    ServerState(boost::asio::io_context& context, std::shared_ptr<RawTcpEvents> events, TcpOptions options)
        : acceptor_(context), events_(events), options_(options)
    {
        validate_configuration(events, options);
    }

    ServerState(boost::asio::io_context& context, std::shared_ptr<TypedEventsAdapter> events, TcpOptions options)
        : ServerState(context, std::shared_ptr<RawTcpEvents>{events}, options)
    {
        typed_events_ = std::move(events);
    }

    ErrorCode start(std::string_view address, std::uint16_t port)
    {
        if (started_ || stopped_)
        {
            return boost::asio::error::already_started;
        }
        started_ = true;
        ErrorCode error;
        const auto ip = boost::asio::ip::make_address(std::string(address), error);
        if (error)
        {
            return error;
        }
        const boost::asio::ip::tcp::endpoint endpoint{ip, port};
        acceptor_.open(endpoint.protocol(), error);
        if (!error)
        {
            acceptor_.bind(endpoint, error);
        }
        if (!error)
        {
            acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
        }
        if (!error)
        {
            port_ = acceptor_.local_endpoint(error).port();
        }
        if (error)
        {
            ErrorCode ignored;
            acceptor_.close(ignored);
            return error;
        }
        accept_next();
        return {};
    }

    void stop()
    {
        if (stopped_)
        {
            return;
        }
        stopped_ = true;
        ErrorCode ignored;
        acceptor_.close(ignored);
        // Removal callbacks can mutate connections_, so iterate a local collection.
        auto connections = std::move(connections_);
        connections_.clear();
        for (const auto& [key, connection] : connections)
        {
            (void)key;
            connection->close();
        }
    }

    std::uint16_t port() const
    {
        return port_;
    }
    void on_connection_closed(TcpConnection* connection) override
    {
        connections_.erase(connection);
    }

private:
    void accept_next()
    {
        acceptor_.async_accept(
            std::bind(&ServerState::on_accepted, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    void on_accepted(ErrorCode error, boost::asio::ip::tcp::socket socket)
    {
        if (stopped_)
        {
            return;
        }
        if (error)
        {
            // Stop first so an error callback can safely destroy the public server.
            stop();
            if (auto events = events_.lock())
            {
                events->on_error("accept", error);
            }
            return;
        }
        auto connection =
            ConnectionState::create(std::move(socket), events_, options_, shared_from_this(), typed_events_);
        connections_.emplace(connection.get(), connection);
        ConnectionState::start(connection);
        if (!stopped_)
        {
            accept_next();
        }
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    std::weak_ptr<RawTcpEvents> events_;
    // Survives callback-driven public-server destruction, including accept-error shutdown.
    // The adapter itself holds only a weak reference to the application observer.
    std::shared_ptr<TypedEventsAdapter> typed_events_;
    TcpOptions options_;
    std::map<TcpConnection*, ConnectionPtr> connections_;
    std::uint16_t port_ = 0;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace tcp_helper::detail

namespace tcp_helper
{

TcpServer::TcpServer(boost::asio::io_context& context, std::shared_ptr<TcpEvents> events, TcpOptions options)
    : state_(std::make_shared<detail::ServerState>(context, detail::make_typed_events(events, options.error_logger),
                                                   options))
{
}

TcpServer::TcpServer(boost::asio::io_context& context, std::shared_ptr<RawTcpEvents> events, TcpOptions options)
    : state_(std::make_shared<detail::ServerState>(context, std::move(events), options))
{
}
TcpServer::~TcpServer()
{
    state_->stop();
}
ErrorCode TcpServer::start(std::string_view address, std::uint16_t port)
{
    return state_->start(address, port);
}
void TcpServer::stop()
{
    const auto state = state_;
    state->stop();
}
std::uint16_t TcpServer::port() const
{
    return state_->port();
}

} // namespace tcp_helper
