#pragma once

#include <tcp_helper/messages.hpp>
#include <memory>
#include <string_view>
#include <system_error>

namespace tcp_helper
{

// send() and is_open() may be called from any thread. True means queued for delivery.
// False means closed or transport limits exceeded. Encoding/allocation errors may throw.
// close() uses the owning transport's event thread, or runs before/after that thread.
class IMessageSender
{
public:
    virtual ~IMessageSender() = default;
    [[nodiscard]] virtual bool send(const Heartbeat& message) = 0;
    [[nodiscard]] virtual bool send(const ShoppingCart& message) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;
};

using MessageSenderPtr = std::shared_ptr<IMessageSender>;

// Retain your observer while callbacks are wanted. Callbacks must not throw.
// Message references are valid for the callback; copy them to retain the values.
// Decoded numeric fields are host values; serialization keeps wire bytes in network order.
class MessageEvents
{
public:
    virtual ~MessageEvents() = default;
    virtual void on_connected(MessageSenderPtr sender) = 0;
    virtual void on_heartbeat(MessageSenderPtr sender, const Heartbeat& message) = 0;
    virtual void on_shopping_cart(MessageSenderPtr sender, const ShoppingCart& message) = 0;
    virtual void on_disconnected(MessageSenderPtr sender, std::error_code reason) = 0;
    virtual void on_error(std::string_view operation, std::error_code error) = 0;
};

} // namespace tcp_helper
