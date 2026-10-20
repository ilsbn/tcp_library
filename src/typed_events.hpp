#pragma once

#include <tcp_helper/tcp.hpp>

namespace tcp_helper::detail
{
class TypedEventsAdapter final : public RawTcpEvents
{
public:
    TypedEventsAdapter(const std::shared_ptr<MessageEvents>& events, std::shared_ptr<IMessageErrorLogger> logger);
    void on_connected(ConnectionPtr connection) override;
    void on_message(ConnectionPtr connection, DataBuffer encoded) override;
    void on_decoded(ConnectionPtr connection, const DecodedMessage& message);
    void on_disconnected(ConnectionPtr connection, ErrorCode reason) override;
    void on_error(std::string_view operation, ErrorCode error) override;

private:
    std::weak_ptr<MessageEvents> events_;
    std::shared_ptr<IMessageErrorLogger> logger_;
};
std::shared_ptr<TypedEventsAdapter> make_typed_events(const std::shared_ptr<MessageEvents>& events,
                                                      std::shared_ptr<IMessageErrorLogger> logger = {});
} // namespace tcp_helper::detail
