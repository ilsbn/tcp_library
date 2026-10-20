#pragma once

#include <tcp_helper/tcp.hpp>
#include "typed_events.hpp"
#include "message_stream_decoder.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <array>
#include <deque>
#include <limits>
#include <stdexcept>
#include <vector>

namespace tcp_helper::detail
{

inline void validate_configuration(const std::shared_ptr<RawTcpEvents>& events, TcpOptions options)
{
    if (!events)
    {
        throw std::invalid_argument("RawTcpEvents must not be null");
    }
    if (options.max_message_size == 0 || options.max_message_size > (std::numeric_limits<std::uint32_t>::max)() ||
        options.max_queued_bytes < 4)
    {
        throw std::invalid_argument("Invalid TCP message or queue limit");
    }
}

class ConnectionOwner
{
public:
    virtual ~ConnectionOwner() = default;
    virtual void on_connection_closed(TcpConnection* connection) = 0;
};

class ConnectionState final : public std::enable_shared_from_this<ConnectionState>
{
public:
    ConnectionState(boost::asio::ip::tcp::socket socket, std::weak_ptr<RawTcpEvents> events, TcpOptions options,
                    std::weak_ptr<ConnectionOwner> owner, std::shared_ptr<TypedEventsAdapter> typed_events);

    static ConnectionPtr create(boost::asio::ip::tcp::socket socket, std::weak_ptr<RawTcpEvents> events,
                                TcpOptions options, std::weak_ptr<ConnectionOwner> owner,
                                std::shared_ptr<TypedEventsAdapter> typed_events = {});
    static void start(const ConnectionPtr& connection);

    bool send(DataBuffer message);
    void close(ErrorCode reason = boost::asio::error::operation_aborted);
    bool is_open() const
    {
        return !closed_;
    }

private:
    void read_header();
    void on_header(ErrorCode error, std::size_t transferred);
    void on_body(ErrorCode error, std::size_t transferred);
    void deliver_message();
    void read_stream();
    void on_stream_bytes(ErrorCode error, std::size_t transferred);
    void dispatch_message(const DecodedMessage& message);
    void write_next();
    void on_write(ErrorCode error, std::size_t transferred);

    boost::asio::ip::tcp::socket socket_;
    std::weak_ptr<RawTcpEvents> events_;
    std::weak_ptr<ConnectionOwner> owner_;
    std::weak_ptr<TcpConnection> connection_;
    TcpOptions options_;
    std::weak_ptr<TypedEventsAdapter> typed_events_;
    std::unique_ptr<MessageStreamDecoder> stream_decoder_;
    std::array<std::uint8_t, MessageStreamDecoder::read_chunk_size> stream_read_buffer_{};
    std::array<unsigned char, 4> header_{};
    std::vector<std::uint8_t> incoming_;
    std::deque<DataBuffer> writes_;
    std::size_t queued_bytes_ = 0;
    bool closed_ = false;
};

} // namespace tcp_helper::detail
