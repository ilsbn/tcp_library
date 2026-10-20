#pragma once

#include <tcp_helper/data_buffer.hpp>
#include <tcp_helper/message_diagnostics.hpp>
#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <variant>

namespace tcp_helper
{

enum class MessageType : std::uint32_t
{
    Heartbeat = 0,
    ShoppingCart = 1
};

using PriceDollar = float;
using SequenceNumber = std::uint64_t;

inline constexpr std::uint64_t message_magic_number = 0x5443504d53473031ull; // TCPMSG01
inline constexpr std::size_t message_header_size = 20;
inline constexpr std::size_t message_checksum_size = 4;

class Header
{
public:
    std::uint64_t m_magic_number = message_magic_number;
    MessageType m_type = MessageType::Heartbeat;
    std::size_t m_size = 0; // Payload bytes; encoded as uint64 on the wire.
};

struct UserIdentifier
{
    std::string m_login_name;
    std::uint64_t m_user_id = 0;

    bool operator==(const UserIdentifier&) const = default;
};

struct Heartbeat
{
    SequenceNumber m_sequence_number = 0;
    UserIdentifier m_user_identifier;

    bool operator==(const Heartbeat&) const = default;
};

struct ProductItem
{
    std::uint64_t m_product_id = 0;
    PriceDollar m_price = 0.0F;

    bool operator==(const ProductItem&) const = default;
};

struct ShoppingCart
{
    UserIdentifier m_user_identifier;
    std::list<ProductItem> m_items;

    bool operator==(const ShoppingCart&) const = default;
};

using MessageBody = std::variant<Heartbeat, ShoppingCart>;

struct DecodedMessage
{
    Header m_header;
    MessageBody m_body;
    std::uint32_t m_checksum = 0;
};

// CRC-32/ISO-HDLC over the supplied bytes; the message trailer covers header + payload.
[[nodiscard]] TCP_HELPER_API std::uint32_t compute_crc32(std::span<const std::uint8_t> bytes);
[[nodiscard]] TCP_HELPER_API DataBuffer encode_message(const Heartbeat& message);
[[nodiscard]] TCP_HELPER_API DataBuffer encode_message(const ShoppingCart& message);
[[nodiscard]] TCP_HELPER_API DataBuffer encode_message(const MessageBody& message);

// Validates the entire byte array, independently of its read position.
// Throws std::invalid_argument for invalid CRC, header, or payload; input is unchanged.
[[nodiscard]] TCP_HELPER_API DecodedMessage decode_message(const DataBuffer& encoded);

} // namespace tcp_helper
