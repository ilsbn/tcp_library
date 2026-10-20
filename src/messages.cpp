#include <tcp_helper/messages.hpp>
#include <boost/crc.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tcp_helper
{

namespace
{
constexpr std::size_t max_encoded_size = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::size_t max_payload_size = max_encoded_size - message_header_size - message_checksum_size;
constexpr std::size_t user_fixed_size = 4 + 8;
constexpr std::size_t product_wire_size = 8 + 4;

[[noreturn]] void fail(MessageErrorCode code, std::size_t offset, std::string_view description,
                       std::optional<std::uint64_t> expected = {}, std::optional<std::uint64_t> actual = {})
{
    throw MessageDecodeError({code, offset, std::string(description), expected, actual});
}

void require_bytes(const DataBuffer& reader, std::size_t count, std::string_view description)
{
    if (reader.remaining() < count)
    {
        fail(MessageErrorCode::truncated_field, reader.read_position(), description, count, reader.remaining());
    }
}

std::size_t user_size(const UserIdentifier& user)
{
    if (user.m_login_name.size() > max_payload_size - user_fixed_size)
    {
        throw std::length_error("Login name does not fit a TCP message");
    }
    return user_fixed_size + user.m_login_name.size();
}

void write_user(DataBuffer& payload, const UserIdentifier& user)
{
    payload.write_uint32(static_cast<std::uint32_t>(user.m_login_name.size()));
    payload.write_bytes(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(user.m_login_name.data()),
                                                      user.m_login_name.size()});
    payload.write_uint64(user.m_user_id);
}

UserIdentifier read_user(DataBuffer& payload)
{
    require_bytes(payload, user_fixed_size, "Truncated user identifier");
    const auto length_offset = payload.read_position();
    const auto name_size = payload.read_uint32();
    if (name_size > payload.remaining() - sizeof(std::uint64_t))
    {
        fail(MessageErrorCode::invalid_login_length, length_offset, "Login name length exceeds the payload",
             payload.remaining() - sizeof(std::uint64_t), name_size);
    }
    const auto name = payload.read_bytes(name_size);
    UserIdentifier user;
    if (!name.empty())
    {
        user.m_login_name.assign(reinterpret_cast<const char*>(name.data()), name.size());
    }
    user.m_user_id = payload.read_uint64();
    return user;
}

DataBuffer wrap_payload(MessageType type, const DataBuffer& payload)
{
    if (payload.size() > max_payload_size)
    {
        throw std::length_error("Encoded message does not fit a TCP frame");
    }
    DataBuffer encoded;
    encoded.reserve(message_header_size + payload.size() + message_checksum_size);
    encoded.write_uint64(message_magic_number);
    encoded.write_uint32(static_cast<std::uint32_t>(type));
    encoded.write_uint64(payload.size());
    encoded.write_bytes(payload.bytes());
    encoded.write_uint32(compute_crc32(encoded.bytes()));
    return encoded;
}

Heartbeat read_heartbeat(DataBuffer& payload)
{
    Heartbeat heartbeat;
    require_bytes(payload, sizeof(SequenceNumber), "Truncated sequence number");
    heartbeat.m_sequence_number = payload.read_uint64();
    heartbeat.m_user_identifier = read_user(payload);
    return heartbeat;
}

ShoppingCart read_cart(DataBuffer& payload)
{
    ShoppingCart cart;
    cart.m_user_identifier = read_user(payload);
    require_bytes(payload, sizeof(std::uint32_t), "Truncated product count");
    const auto count_offset = payload.read_position();
    const auto count = payload.read_uint32();
    // Validate the complete list length before allocating any list nodes.
    if (count > payload.remaining() / product_wire_size)
    {
        fail(MessageErrorCode::invalid_item_count, count_offset, "Product count exceeds the payload",
             payload.remaining() / product_wire_size, count);
    }
    for (std::uint32_t index = 0; index < count; ++index)
    {
        ProductItem item;
        item.m_product_id = payload.read_uint64();
        item.m_price = payload.read_float32();
        cart.m_items.push_back(item);
    }
    return cart;
}
} // namespace

std::uint32_t compute_crc32(std::span<const std::uint8_t> bytes)
{
    boost::crc_32_type crc;
    if (!bytes.empty())
    {
        crc.process_bytes(bytes.data(), bytes.size());
    }
    return static_cast<std::uint32_t>(crc.checksum());
}

DataBuffer encode_message(const Heartbeat& message)
{
    const auto identifier_size = user_size(message.m_user_identifier);
    if (identifier_size > max_payload_size - sizeof(SequenceNumber))
    {
        throw std::length_error("Heartbeat does not fit a TCP frame");
    }
    DataBuffer payload;
    payload.reserve(sizeof(SequenceNumber) + identifier_size);
    payload.write_uint64(message.m_sequence_number);
    write_user(payload, message.m_user_identifier);
    return wrap_payload(MessageType::Heartbeat, payload);
}

DataBuffer encode_message(const ShoppingCart& message)
{
    const auto identifier_size = user_size(message.m_user_identifier);
    if (identifier_size > max_payload_size - sizeof(std::uint32_t) ||
        message.m_items.size() > (max_payload_size - identifier_size - sizeof(std::uint32_t)) / product_wire_size)
    {
        throw std::length_error("Shopping cart does not fit a TCP frame");
    }
    DataBuffer payload;
    payload.reserve(identifier_size + sizeof(std::uint32_t) + message.m_items.size() * product_wire_size);
    write_user(payload, message.m_user_identifier);
    payload.write_uint32(static_cast<std::uint32_t>(message.m_items.size()));
    for (const auto& item : message.m_items)
    {
        payload.write_uint64(item.m_product_id);
        payload.write_float32(item.m_price);
    }
    return wrap_payload(MessageType::ShoppingCart, payload);
}

DataBuffer encode_message(const MessageBody& message)
{
    if (const auto* heartbeat = std::get_if<Heartbeat>(&message))
    {
        return encode_message(*heartbeat);
    }
    return encode_message(std::get<ShoppingCart>(message));
}

DecodedMessage decode_message(const DataBuffer& encoded)
{
    if (encoded.size() < message_header_size + message_checksum_size || encoded.size() > max_encoded_size)
    {
        fail(MessageErrorCode::invalid_message_size, 0, "Invalid encoded message size",
             encoded.size() > max_encoded_size ? max_encoded_size : message_header_size + message_checksum_size,
             encoded.size());
    }
    const auto content = encoded.bytes().first(encoded.size() - message_checksum_size);
    DataBuffer trailer{encoded.bytes().last(message_checksum_size)};
    const auto checksum = trailer.read_uint32();
    const auto expected_checksum = compute_crc32(content);
    if (expected_checksum != checksum)
    {
        fail(MessageErrorCode::checksum_mismatch, content.size(), "Message checksum mismatch", expected_checksum,
             checksum);
    }

    DataBuffer reader{content};
    Header header;
    header.m_magic_number = reader.read_uint64();
    header.m_type = static_cast<MessageType>(reader.read_uint32());
    const auto payload_size = reader.read_uint64();
    if (header.m_magic_number != message_magic_number)
    {
        fail(MessageErrorCode::invalid_magic, 0, "Unknown message magic number", message_magic_number,
             header.m_magic_number);
    }
    if (payload_size != reader.remaining())
    {
        fail(MessageErrorCode::payload_size_mismatch, 12, "Message payload size mismatch", reader.remaining(),
             payload_size);
    }
    header.m_size = static_cast<std::size_t>(payload_size);
    MessageBody body;
    switch (header.m_type)
    {
    case MessageType::Heartbeat:
        body = read_heartbeat(reader);
        break;
    case MessageType::ShoppingCart:
        body = read_cart(reader);
        break;
    default:
        fail(MessageErrorCode::unknown_message_type, 8, "Unknown message type", {},
             static_cast<std::uint32_t>(header.m_type));
    }
    if (reader.remaining() != 0)
    {
        fail(MessageErrorCode::trailing_payload, reader.read_position(), "Unexpected trailing message payload", 0,
             reader.remaining());
    }
    return {header, std::move(body), checksum};
}

} // namespace tcp_helper
