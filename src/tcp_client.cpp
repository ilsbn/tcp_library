#include "connection_state.hpp"
#include "typed_events.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <spdlog/spdlog.h>
#include <functional>

namespace tcp_helper::detail
{

class ClientState final : public ConnectionOwner, public std::enable_shared_from_this<ClientState>
{
    enum class RequestedState
    {
        idle,
        started,
        stopped
    };

public:
    ClientState(boost::asio::io_context& context, std::shared_ptr<RawTcpEvents> events, TcpOptions options)
        : resolver_(context), socket_(context), executor_(socket_.get_executor()), events_(events), options_(options)
    {
        validate_configuration(events, options);
    }

    ClientState(boost::asio::io_context& context, std::shared_ptr<TypedEventsAdapter> events, TcpOptions options)
        : ClientState(context, std::shared_ptr<RawTcpEvents>{events}, options)
    {
        typed_events_ = std::move(events);
    }

    void connect(std::string host, std::uint16_t port)
    {
        auto expected = RequestedState::idle;
        if (!requested_state_.compare_exchange_strong(expected, RequestedState::started))
        {
            throw std::logic_error("TcpClient connects once; create a new client to reconnect");
        }
        boost::asio::dispatch(executor_,
                              std::bind(&ClientState::begin_connect, shared_from_this(), std::move(host), port));
    }

    void stop()
    {
        if (requested_state_.exchange(RequestedState::stopped) != RequestedState::stopped)
        {
            boost::asio::dispatch(executor_, std::bind(&ClientState::on_stop, shared_from_this()));
        }
    }

    void on_connection_closed(TcpConnection*) override
    {
        connection_.reset();
    }

private:
    void begin_connect(const std::string& host, std::uint16_t port)
    {
        if (requested_state_ == RequestedState::stopped)
        {
            return;
        }
        resolver_.async_resolve(
            host, std::to_string(port),
            std::bind(&ClientState::on_resolved, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    void on_stop()
    {
        if (stopped_)
        {
            return;
        }
        stopped_ = true;
        resolver_.cancel();
        ErrorCode ignored;
        socket_.close(ignored);
        if (const auto connection = connection_)
        {
            connection->close();
        }
    }

    void on_resolved(ErrorCode error, boost::asio::ip::tcp::resolver::results_type endpoints)
    {
        if (requested_state_ == RequestedState::stopped)
        {
            return;
        }
        if (error)
        {
            report_error("resolve", error);
            return;
        }
        boost::asio::async_connect(
            socket_, endpoints,
            std::bind(&ClientState::on_connected, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    void on_connected(ErrorCode error, const boost::asio::ip::tcp::endpoint&)
    {
        if (requested_state_ == RequestedState::stopped)
        {
            return;
        }
        if (error)
        {
            report_error("connect", error);
            return;
        }
        connection_ = ConnectionState::create(std::move(socket_), events_, options_, shared_from_this(), typed_events_);
        // Keep a local copy: on_connected can call stop(), resetting connection_.
        const auto connection = connection_;
        ConnectionState::start(connection);
    }

    void report_error(std::string_view operation, ErrorCode error)
    {
        spdlog::debug("TCP {} failed: {}", operation, error.message());
        if (auto events = events_.lock())
        {
            events->on_error(operation, error);
        }
    }

    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    const boost::asio::ip::tcp::socket::executor_type executor_;
    std::weak_ptr<RawTcpEvents> events_;
    // Only the typed adapter is retained; it holds a weak application observer.
    std::shared_ptr<TypedEventsAdapter> typed_events_;
    TcpOptions options_;
    ConnectionPtr connection_;
    std::atomic<RequestedState> requested_state_ = RequestedState::idle;
    bool stopped_ = false;
};

} // namespace tcp_helper::detail

namespace tcp_helper
{

TcpClient::TcpClient(boost::asio::io_context& context, std::shared_ptr<TcpEvents> events, TcpOptions options)
    : state_(std::make_shared<detail::ClientState>(context, detail::make_typed_events(events, options.error_logger),
                                                   options))
{
}

TcpClient::TcpClient(boost::asio::io_context& context, std::shared_ptr<RawTcpEvents> events, TcpOptions options)
    : state_(std::make_shared<detail::ClientState>(context, std::move(events), options))
{
}
TcpClient::~TcpClient()
{
    state_->stop();
}
void TcpClient::connect(std::string host, std::uint16_t port)
{
    state_->connect(std::move(host), port);
}
void TcpClient::stop()
{
    const auto state = state_;
    state->stop();
}

} // namespace tcp_helper
