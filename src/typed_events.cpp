#include "typed_events.hpp"

namespace tcp_helper::detail
{
TypedEventsAdapter::TypedEventsAdapter(const std::shared_ptr<MessageEvents>& events,
                                       std::shared_ptr<IMessageErrorLogger> logger)
    : events_(events), logger_(logger ? std::move(logger) : default_message_error_logger())
{
}
void TypedEventsAdapter::on_connected(ConnectionPtr connection)
{
    if (auto events = events_.lock())
    {
        events->on_connected(std::move(connection));
    }
}
void TypedEventsAdapter::on_message(ConnectionPtr connection, DataBuffer encoded)
{
    DecodedMessage message;
    try
    {
        message = decode_message(encoded);
    }
    catch (const MessageDecodeError& error)
    {
        auto details = error.details();
        details.received_size = encoded.size();
        logger_->log(details);
        return;
    }
    on_decoded(std::move(connection), message);
}
void TypedEventsAdapter::on_decoded(ConnectionPtr connection, const DecodedMessage& message)
{
    if (auto events = events_.lock())
    {
        if (const auto* heartbeat = std::get_if<Heartbeat>(&message.m_body))
        {
            events->on_heartbeat(std::move(connection), *heartbeat);
        }
        else
        {
            events->on_shopping_cart(std::move(connection), std::get<ShoppingCart>(message.m_body));
        }
    }
}
void TypedEventsAdapter::on_disconnected(ConnectionPtr connection, ErrorCode reason)
{
    if (auto events = events_.lock())
    {
        events->on_disconnected(std::move(connection), reason);
    }
}
void TypedEventsAdapter::on_error(std::string_view operation, ErrorCode error)
{
    if (auto events = events_.lock())
    {
        events->on_error(operation, error);
    }
}
std::shared_ptr<TypedEventsAdapter> make_typed_events(const std::shared_ptr<MessageEvents>& events,
                                                      std::shared_ptr<IMessageErrorLogger> logger)
{
    if (!events)
    {
        throw std::invalid_argument("MessageEvents must not be null");
    }
    return std::make_shared<TypedEventsAdapter>(events, std::move(logger));
}
} // namespace tcp_helper::detail
