#include <tcp_helper/messages.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace tcp_helper;
using testing::ElementsAreArray;

namespace
{
// Independent fixtures: fields packed with Python struct, CRC checked with zlib.crc32.
const std::array<std::uint8_t, 45> heartbeat_wire{
    0x54, 0x43, 0x50, 0x4d, 0x53, 0x47, 0x30, 0x31, 0, 0,    0, 0, 0, 0, 0, 0, 0, 0, 0,    0x15, 0,    0,   0,
    0,    0,    0,    0,    1,    0,    0,    0,    1, 0x41, 0, 0, 0, 0, 0, 0, 0, 2, 0xd3, 0x52, 0xca, 0x3d};

const std::array<std::uint8_t, 65> cart_wire{
    0x54, 0x43, 0x50, 0x4d, 0x53, 0x47, 0x30, 0x31, 0, 0, 0, 1, 0, 0,    0,    0, 0, 0,    0,    0x29, 0,   0,
    0,    1,    0x41, 0,    0,    0,    0,    0,    0, 0, 2, 0, 0, 0,    2,    0, 0, 0,    0,    0,    0,   0,
    3,    0x3f, 0xc0, 0,    0,    0,    0,    0,    0, 0, 0, 0, 4, 0x40, 0x10, 0, 0, 0xb1, 0x6b, 0x1c, 0x68};

DataBuffer renew_checksum(std::vector<std::uint8_t> bytes)
{
    bytes.resize(bytes.size() - message_checksum_size);
    DataBuffer result{std::move(bytes)};
    result.write_uint32(compute_crc32(result.bytes()));
    return result;
}

DataBuffer make_packet(MessageType type, const DataBuffer& payload)
{
    DataBuffer encoded;
    encoded.write_uint64(message_magic_number);
    encoded.write_uint32(static_cast<std::uint32_t>(type));
    encoded.write_uint64(payload.size());
    encoded.write_bytes(payload.bytes());
    encoded.write_uint32(compute_crc32(encoded.bytes()));
    return encoded;
}

void validate_packet(const DataBuffer& encoded)
{
    static_cast<void>(decode_message(encoded));
}
} // namespace

TEST(MessagesTest, Crc32MatchesStandardCheckVector)
{
    const std::array<std::uint8_t, 9> bytes{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(compute_crc32(bytes), 0xcbf43926u);
    EXPECT_EQ(compute_crc32({}), 0u);
}

TEST(MessagesTest, EncodesHeartbeatAsKnownWireBytes)
{
    const Heartbeat message{1, {"A", 2}};
    EXPECT_THAT(encode_message(message).bytes(), ElementsAreArray(heartbeat_wire));
}

TEST(MessagesTest, DecodesKnownHeartbeatIncludingHeaderAndChecksum)
{
    const auto decoded = decode_message(DataBuffer{heartbeat_wire});
    EXPECT_EQ(decoded.m_header.m_magic_number, 0x5443504d53473031ull);
    EXPECT_EQ(decoded.m_header.m_type, MessageType::Heartbeat);
    EXPECT_EQ(decoded.m_header.m_size, 21u);
    EXPECT_EQ(decoded.m_checksum, 0xd352ca3du);
    EXPECT_EQ(std::get<Heartbeat>(decoded.m_body), (Heartbeat{1, {"A", 2}}));
}

TEST(MessagesTest, EncodesShoppingCartAsKnownWireBytes)
{
    const ShoppingCart message{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}};
    EXPECT_THAT(encode_message(message).bytes(), ElementsAreArray(cart_wire));
}

TEST(MessagesTest, DecodesKnownShoppingCartIncludingItemsAndPrices)
{
    const auto decoded = decode_message(DataBuffer{cart_wire});
    EXPECT_EQ(decoded.m_header.m_type, MessageType::ShoppingCart);
    EXPECT_EQ(decoded.m_header.m_size, 41u);
    EXPECT_EQ(decoded.m_checksum, 0xb16b1c68u);
    EXPECT_EQ(std::get<ShoppingCart>(decoded.m_body), (ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}}));
}

TEST(MessagesTest, RoundTripsEmptyAndBinaryLoginNamesAndMaximumIds)
{
    const Heartbeat heartbeat{(std::numeric_limits<SequenceNumber>::max)(),
                              {std::string("a\0b", 3), (std::numeric_limits<std::uint64_t>::max)()}};
    EXPECT_EQ(std::get<Heartbeat>(decode_message(encode_message(heartbeat)).m_body), heartbeat);
    const ShoppingCart cart{{"", 0}, {}};
    EXPECT_EQ(std::get<ShoppingCart>(decode_message(encode_message(cart)).m_body), cart);
}

TEST(MessagesTest, VariantEncodingDispatchesBothMessageTypes)
{
    const MessageBody heartbeat = Heartbeat{1, {"A", 2}};
    const MessageBody cart = ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}};
    EXPECT_THAT(encode_message(heartbeat).bytes(), ElementsAreArray(heartbeat_wire));
    EXPECT_THAT(encode_message(cart).bytes(), ElementsAreArray(cart_wire));
}

TEST(MessagesTest, DecodeUsesAllBytesAndPreservesInputReadPosition)
{
    DataBuffer encoded{heartbeat_wire};
    encoded.read_uint64();
    const auto decoded = decode_message(encoded);
    EXPECT_EQ(std::get<Heartbeat>(decoded.m_body).m_sequence_number, 1u);
    EXPECT_EQ(encoded.read_position(), 8u);
}

TEST(MessagesTest, DetectsCorruptionInEveryHeaderPayloadAndChecksumByte)
{
    for (std::size_t index = 0; index < cart_wire.size(); ++index)
    {
        SCOPED_TRACE(index);
        auto bytes = std::vector<std::uint8_t>(cart_wire.begin(), cart_wire.end());
        bytes[index] ^= 1;
        EXPECT_THROW(validate_packet(DataBuffer{std::move(bytes)}), std::invalid_argument);
    }
}

TEST(MessagesTest, RejectsEveryTruncationAndAppendedBytes)
{
    for (std::size_t size = 0; size < heartbeat_wire.size(); ++size)
    {
        SCOPED_TRACE(size);
        EXPECT_THROW(validate_packet(DataBuffer{std::span<const std::uint8_t>{heartbeat_wire}.first(size)}),
                     std::invalid_argument);
    }
    auto bytes = std::vector<std::uint8_t>(heartbeat_wire.begin(), heartbeat_wire.end());
    bytes.push_back(0);
    EXPECT_THROW(validate_packet(DataBuffer{std::move(bytes)}), std::invalid_argument);
}

TEST(MessagesTest, RejectsUnknownMagicEvenWithValidChecksum)
{
    auto bytes = std::vector<std::uint8_t>(heartbeat_wire.begin(), heartbeat_wire.end());
    bytes[0] ^= 1;
    EXPECT_THROW(validate_packet(renew_checksum(std::move(bytes))), std::invalid_argument);
}

TEST(MessagesTest, RejectsUnknownTypeEvenWithValidChecksum)
{
    auto bytes = std::vector<std::uint8_t>(heartbeat_wire.begin(), heartbeat_wire.end());
    bytes[11] = 99;
    EXPECT_THROW(validate_packet(renew_checksum(std::move(bytes))), std::invalid_argument);
}

TEST(MessagesTest, RejectsIncorrectPayloadSizeEvenWithValidChecksum)
{
    auto bytes = std::vector<std::uint8_t>(heartbeat_wire.begin(), heartbeat_wire.end());
    bytes[12] = 0xff;
    EXPECT_THROW(validate_packet(renew_checksum(std::move(bytes))), std::invalid_argument);
}

TEST(MessagesTest, RejectsLoginLengthBeforeAllocatingFromIt)
{
    DataBuffer payload;
    payload.write_uint64(1);
    payload.write_uint32(0xffffffffu);
    payload.write_uint64(2);
    EXPECT_THROW(validate_packet(make_packet(MessageType::Heartbeat, payload)), std::invalid_argument);
}

TEST(MessagesTest, RejectsItemCountBeforeAllocatingListNodes)
{
    DataBuffer payload;
    payload.write_uint32(0);
    payload.write_uint64(1);
    payload.write_uint32(0xffffffffu);
    EXPECT_THROW(validate_packet(make_packet(MessageType::ShoppingCart, payload)), std::invalid_argument);
}

TEST(MessagesTest, RejectsMissingFieldsAndTrailingPayloadWithValidChecksums)
{
    EXPECT_THROW(validate_packet(make_packet(MessageType::Heartbeat, DataBuffer{})), std::invalid_argument);
    DataBuffer payload;
    payload.write_uint64(1);
    payload.write_uint32(0);
    payload.write_uint64(2);
    payload.write_uint8(0);
    EXPECT_THROW(validate_packet(make_packet(MessageType::Heartbeat, payload)), std::invalid_argument);
    DataBuffer cart;
    cart.write_uint32(0);
    cart.write_uint64(1);
    cart.write_uint32(1);
    cart.write_uint64(2);
    EXPECT_THROW(validate_packet(make_packet(MessageType::ShoppingCart, cart)), std::invalid_argument);
}

TEST(MessagesTest, PreservesSignedZeroPriceBits)
{
    const ShoppingCart cart{{"alice", 42}, {{1, std::bit_cast<float>(0x80000000u)}}};
    const auto decoded = decode_message(encode_message(cart));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(std::get<ShoppingCart>(decoded.m_body).m_items.front().m_price),
              0x80000000u);
}
