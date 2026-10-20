#pragma once

#include <tcp_helper/export.hpp>
#include <tcp_helper/data_buffer.hpp>
#include <tcp_helper/message_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tcp_helper
{

namespace detail
{
class ConnectionState;
class ClientState;
class ServerState;
class TypedEventsAdapter;
}

using ErrorCode = boost::system::error_code;
class TcpConnection;
using ConnectionPtr = std::shared_ptr<TcpConnection>;
using TcpEvents = MessageEvents;

struct TcpOptions
{
    std::size_t max_message_size = 1024 * 1024;
    std::size_t max_queued_bytes = 4 * 1024 * 1024;
    std::shared_ptr<IMessageErrorLogger> error_logger = {};
};

// Retain your observer while callbacks are wanted. Observers must not throw.
class RawTcpEvents
{
public:
    virtual ~RawTcpEvents() = default;
    virtual void on_connected(ConnectionPtr connection) = 0;
    virtual void on_message(ConnectionPtr connection, DataBuffer message) = 0;
    virtual void on_disconnected(ConnectionPtr connection, ErrorCode reason) = 0;
    virtual void on_error(std::string_view operation, ErrorCode error) = 0;
};

// All methods/destruction use the one thread running the supplied io_context.
class TcpConnection final : public IMessageSender
{
public:
    TCP_HELPER_API ~TcpConnection() override;
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // Queues all bytes, independent of the buffer's read position.
    // False means closed, too large, or queue full.
    [[nodiscard]] TCP_HELPER_API bool send(DataBuffer message);
    [[nodiscard]] TCP_HELPER_API bool send(const Heartbeat& message) override;
    [[nodiscard]] TCP_HELPER_API bool send(const ShoppingCart& message) override;
    TCP_HELPER_API void close() override;
    [[nodiscard]] TCP_HELPER_API bool is_open() const override;

private:
    friend class detail::ConnectionState;
    explicit TcpConnection(std::shared_ptr<detail::ConnectionState> state);
    std::shared_ptr<detail::ConnectionState> state_;
};

class TcpClient final
{
public:
    TCP_HELPER_API TcpClient(boost::asio::io_context& context, std::shared_ptr<TcpEvents> events,
                             TcpOptions options = {});
    TCP_HELPER_API TcpClient(boost::asio::io_context& context, std::shared_ptr<RawTcpEvents> events,
                             TcpOptions options = {});
    TCP_HELPER_API ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // Each instance connects once; repeated calls throw std::logic_error.
    // Network failures arrive through on_error().
    TCP_HELPER_API void connect(std::string host, std::uint16_t port);
    TCP_HELPER_API void stop();

private:
    std::shared_ptr<detail::ClientState> state_;
};

class TcpServer final
{
public:
    TCP_HELPER_API TcpServer(boost::asio::io_context& context, std::shared_ptr<TcpEvents> events,
                             TcpOptions options = {});
    TCP_HELPER_API TcpServer(boost::asio::io_context& context, std::shared_ptr<RawTcpEvents> events,
                             TcpOptions options = {});
    TCP_HELPER_API ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Numeric IPv4/IPv6 address. Port zero chooses an available port.
    // Returns setup errors synchronously; each instance starts once.
    [[nodiscard]] TCP_HELPER_API ErrorCode start(std::string_view address, std::uint16_t port);
    TCP_HELPER_API void stop();
    [[nodiscard]] TCP_HELPER_API std::uint16_t port() const;

private:
    std::shared_ptr<detail::ServerState> state_;
};

} // namespace tcp_helper
