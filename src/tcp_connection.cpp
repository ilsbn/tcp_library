#include "connection_state.hpp"
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/endian/conversion.hpp>
#include <spdlog/spdlog.h>
#include <functional>
#include <atomic>

namespace tcp_helper::detail
{
namespace
{
std::atomic<std::uint64_t> next_connection_id{1};
}

ConnectionState::ConnectionState(boost::asio::ip::tcp::socket socket, std::weak_ptr<RawTcpEvents> events,
                                 TcpOptions options, std::weak_ptr<ConnectionOwner> owner,
                                 std::shared_ptr<TypedEventsAdapter> typed_events)
    : socket_(std::move(socket)), events_(std::move(events)), owner_(std::move(owner)), options_(options),
      typed_events_(typed_events)
{
    if (typed_events)
    {
        stream_decoder_ = std::make_unique<MessageStreamDecoder>(options.max_message_size, options.error_logger,
                                                                 next_connection_id.fetch_add(1));
    }
}

ConnectionPtr ConnectionState::create(boost::asio::ip::tcp::socket socket, std::weak_ptr<RawTcpEvents> events,
                                      TcpOptions options, std::weak_ptr<ConnectionOwner> owner,
                                      std::shared_ptr<TypedEventsAdapter> typed_events)
{
    auto state = std::make_shared<ConnectionState>(std::move(socket), std::move(events), options, std::move(owner),
                                                   std::move(typed_events));
    auto connection = ConnectionPtr(new TcpConnection(state));
    state->connection_ = connection;
    return connection;
}

void ConnectionState::start(const ConnectionPtr& connection)
{
    // Callbacks can synchronously close the connection or its client/server.
    const auto state = connection->state_;
    if (auto events = state->events_.lock())
    {
        events->on_connected(connection);
    }
    if (!state->closed_)
    {
        if (state->stream_decoder_)
        {
            state->read_stream();
        }
        else
        {
            state->read_header();
        }
    }
}

bool ConnectionState::send(DataBuffer message)
{
    if (closed_ || message.size() > options_.max_message_size)
    {
        return false;
    }
    const auto frame_size = message.size() + header_.size();
    if (frame_size > options_.max_queued_bytes - queued_bytes_)
    {
        return false;
    }

    DataBuffer frame;
    frame.reserve(frame_size);
    frame.write_uint32(static_cast<std::uint32_t>(message.size()));
    frame.write_bytes(message.bytes());
    writes_.push_back(std::move(frame));
    queued_bytes_ += frame_size;
    if (writes_.size() == 1)
    {
        write_next();
    }
    return true;
}

void ConnectionState::close(ErrorCode reason)
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    ErrorCode ignored;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    // Do not clear incoming_/writes_: canceled operations still refer to them.
    const auto connection = connection_.lock();
    if (auto owner = owner_.lock())
    {
        owner->on_connection_closed(connection.get());
    }
    spdlog::debug("TCP connection closed: {}", reason.message());
    if (auto events = events_.lock(); events && connection)
    {
        events->on_disconnected(connection, reason);
    }
}

void ConnectionState::read_header()
{
    boost::asio::async_read(
        socket_, boost::asio::buffer(header_),
        std::bind(&ConnectionState::on_header, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void ConnectionState::read_stream()
{
    socket_.async_read_some(
        boost::asio::buffer(stream_read_buffer_),
        std::bind(&ConnectionState::on_stream_bytes, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void ConnectionState::on_stream_bytes(ErrorCode error, std::size_t transferred)
{
    if (closed_)
    {
        return;
    }
    stream_decoder_->append(std::span<const std::uint8_t>{stream_read_buffer_}.first(transferred));
    while (!closed_)
    {
        const auto message = stream_decoder_->next_message();
        if (!message || closed_)
        {
            break;
        }
        dispatch_message(*message);
    }
    if (closed_)
    {
        return;
    }
    if (error)
    {
        for (const auto& message : stream_decoder_->finish())
        {
            dispatch_message(message);
        }
        close(error);
        return;
    }
    read_stream();
}

void ConnectionState::dispatch_message(const DecodedMessage& message)
{
    if (!closed_)
    {
        if (auto events = typed_events_.lock())
        {
            if (auto connection = connection_.lock())
            {
                events->on_decoded(std::move(connection), message);
            }
        }
    }
}

void ConnectionState::on_header(ErrorCode error, std::size_t)
{
    if (closed_)
    {
        return;
    }
    if (error)
    {
        close(error);
        return;
    }
    const auto length = boost::endian::endian_load<std::uint32_t, 4, boost::endian::order::big>(header_.data());
    if (length > options_.max_message_size)
    {
        close(boost::asio::error::message_size);
        return;
    }
    incoming_.assign(length, 0);
    if (incoming_.empty())
    {
        deliver_message();
        return;
    }
    boost::asio::async_read(
        socket_, boost::asio::buffer(incoming_),
        std::bind(&ConnectionState::on_body, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void ConnectionState::on_body(ErrorCode error, std::size_t)
{
    if (closed_)
    {
        return;
    }
    if (error)
    {
        close(error);
        return;
    }
    deliver_message();
}

void ConnectionState::deliver_message()
{
    if (auto events = events_.lock())
    {
        if (auto connection = connection_.lock())
        {
            events->on_message(connection, DataBuffer{std::move(incoming_)});
        }
    }
    if (!closed_)
    {
        read_header();
    }
}

void ConnectionState::write_next()
{
    boost::asio::async_write(
        socket_, boost::asio::buffer(writes_.front().bytes().data(), writes_.front().size()),
        std::bind(&ConnectionState::on_write, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void ConnectionState::on_write(ErrorCode error, std::size_t)
{
    if (closed_ || error)
    {
        writes_.clear();
        queued_bytes_ = 0;
        if (!closed_)
        {
            close(error);
        }
        return;
    }
    queued_bytes_ -= writes_.front().size();
    writes_.pop_front();
    if (!writes_.empty())
    {
        write_next();
    }
}

} // namespace tcp_helper::detail

namespace tcp_helper
{

TcpConnection::TcpConnection(std::shared_ptr<detail::ConnectionState> state) : state_(std::move(state))
{
}
TcpConnection::~TcpConnection() = default;
bool TcpConnection::send(const Heartbeat& message)
{
    return state_->is_open() && state_->send(encode_message(message));
}
bool TcpConnection::send(const ShoppingCart& message)
{
    return state_->is_open() && state_->send(encode_message(message));
}
bool TcpConnection::send(DataBuffer message)
{
    return state_->send(std::move(message));
}
void TcpConnection::close()
{
    const auto state = state_;
    state->close();
}
bool TcpConnection::is_open() const
{
    return state_->is_open();
}

} // namespace tcp_helper
