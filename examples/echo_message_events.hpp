#pragma once

#include <tcp_helper/message_channel.hpp>
#include <spdlog/spdlog.h>

// Application handler: no TCP, sockets, io_context, or byte encoding.
class EchoMessageEvents : public tcp_helper::MessageEvents
{
public:
    void on_connected(tcp_helper::MessageSenderPtr) override
    {
    }
    void on_heartbeat(tcp_helper::MessageSenderPtr sender, const tcp_helper::Heartbeat& message) override
    {
        spdlog::info("Echoing Heartbeat: sequence={}, login={}", message.m_sequence_number,
                     message.m_user_identifier.m_login_name);
        if (!sender->send(message))
        {
            failed = true;
            sender->close();
        }
    }
    void on_shopping_cart(tcp_helper::MessageSenderPtr sender, const tcp_helper::ShoppingCart& message) override
    {
        spdlog::info("Echoing ShoppingCart: login={}, items={}", message.m_user_identifier.m_login_name,
                     message.m_items.size());
        if (!sender->send(message))
        {
            failed = true;
            sender->close();
        }
    }
    void on_disconnected(tcp_helper::MessageSenderPtr, std::error_code) override
    {
    }
    void on_error(std::string_view operation, std::error_code error) override
    {
        failed = true;
        spdlog::error("{}: {}", operation, error.message());
    }
    bool failed = false;
};
