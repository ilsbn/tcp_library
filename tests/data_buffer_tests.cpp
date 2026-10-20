#include <tcp_helper/data_buffer.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

using tcp_helper::ByteOrder;
using tcp_helper::DataBuffer;
using testing::ElementsAre;

// Literal byte fixtures catch codecs that round-trip with the same endian bug.
TEST(DataBufferTest, WritesUnsignedFieldsInNetworkOrderByDefault)
{
    DataBuffer buffer;
    buffer.write_uint8(0xab);
    buffer.write_uint16(0x1234);
    buffer.write_uint32(0x89abcdef);
    buffer.write_uint64(0x0123456789abcdef);
    EXPECT_THAT(buffer.bytes(),
                ElementsAre(0xab, 0x12, 0x34, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef));
    EXPECT_EQ(buffer.read_position(), 0u);
}

TEST(DataBufferTest, WritesUnsignedFieldsInLittleEndianOrder)
{
    DataBuffer buffer;
    buffer.write_uint8(0xab, ByteOrder::little_endian);
    buffer.write_uint16(0x1234, ByteOrder::little_endian);
    buffer.write_uint32(0x89abcdef, ByteOrder::little_endian);
    buffer.write_uint64(0x0123456789abcdef, ByteOrder::little_endian);
    EXPECT_THAT(buffer.bytes(),
                ElementsAre(0xab, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01));
}

TEST(DataBufferTest, ReadsKnownUnsignedNetworkBytes)
{
    DataBuffer buffer{std::vector<std::uint8_t>{0xab, 0x12, 0x34, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
                                                0xab, 0xcd, 0xef}};
    EXPECT_EQ(buffer.read_uint8(), 0xab);
    EXPECT_EQ(buffer.read_uint16(), 0x1234);
    EXPECT_EQ(buffer.read_uint32(), 0x89abcdefu);
    EXPECT_EQ(buffer.read_uint64(), 0x0123456789abcdefull);
    EXPECT_EQ(buffer.remaining(), 0u);
}

TEST(DataBufferTest, ReadsKnownUnsignedLittleEndianBytes)
{
    DataBuffer buffer{std::vector<std::uint8_t>{0xab, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89, 0xef, 0xcd, 0xab, 0x89, 0x67,
                                                0x45, 0x23, 0x01}};
    EXPECT_EQ(buffer.read_uint8(ByteOrder::little_endian), 0xab);
    EXPECT_EQ(buffer.read_uint16(ByteOrder::little_endian), 0x1234);
    EXPECT_EQ(buffer.read_uint32(ByteOrder::little_endian), 0x89abcdefu);
    EXPECT_EQ(buffer.read_uint64(ByteOrder::little_endian), 0x0123456789abcdefull);
    EXPECT_EQ(buffer.remaining(), 0u);
}

TEST(DataBufferTest, SupportsMixedByteOrdersWithinOneMessage)
{
    DataBuffer buffer;
    buffer.write_uint32(0x12345678);
    buffer.write_int16(-2, ByteOrder::little_endian);
    buffer.write_uint16(0x0102);
    EXPECT_THAT(buffer.bytes(), ElementsAre(0x12, 0x34, 0x56, 0x78, 0xfe, 0xff, 1, 2));
    EXPECT_EQ(buffer.read_uint32(), 0x12345678u);
    EXPECT_EQ(buffer.read_int16(ByteOrder::little_endian), -2);
    EXPECT_EQ(buffer.read_uint16(), 0x0102);
}

TEST(DataBufferTest, PreservesSignedLimitsInBothByteOrders)
{
    for (const auto order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        DataBuffer buffer;
        buffer.write_int8((std::numeric_limits<std::int8_t>::min)(), order);
        buffer.write_int16((std::numeric_limits<std::int16_t>::min)(), order);
        buffer.write_int32((std::numeric_limits<std::int32_t>::min)(), order);
        buffer.write_int64((std::numeric_limits<std::int64_t>::min)(), order);
        buffer.write_int8((std::numeric_limits<std::int8_t>::max)(), order);
        buffer.write_int16((std::numeric_limits<std::int16_t>::max)(), order);
        buffer.write_int32((std::numeric_limits<std::int32_t>::max)(), order);
        buffer.write_int64((std::numeric_limits<std::int64_t>::max)(), order);
        EXPECT_EQ(buffer.read_int8(order), -128);
        EXPECT_EQ(buffer.read_int16(order), -32768);
        EXPECT_EQ(buffer.read_int32(order), (std::numeric_limits<std::int32_t>::min)());
        EXPECT_EQ(buffer.read_int64(order), (std::numeric_limits<std::int64_t>::min)());
        EXPECT_EQ(buffer.read_int8(order), 127);
        EXPECT_EQ(buffer.read_int16(order), 32767);
        EXPECT_EQ(buffer.read_int32(order), 2147483647);
        EXPECT_EQ(buffer.read_int64(order), 9223372036854775807ll);
    }
}

TEST(DataBufferTest, ReadsAndWritesSignedTwosComplementBytes)
{
    DataBuffer buffer;
    buffer.write_int8(-1);
    buffer.write_int16(-2);
    buffer.write_int32(-3, ByteOrder::little_endian);
    buffer.write_int64(-4);
    EXPECT_THAT(buffer.bytes(),
                ElementsAre(0xff, 0xff, 0xfe, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfc));
    DataBuffer known{std::vector<std::uint8_t>{0xff, 0xfe, 0xff, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                               0xff, 0xff, 0xfc}};
    EXPECT_EQ(known.read_int8(), -1);
    EXPECT_EQ(known.read_int16(ByteOrder::little_endian), -2);
    EXPECT_EQ(known.read_int32(ByteOrder::little_endian), -3);
    EXPECT_EQ(known.read_int64(), -4);
}

TEST(DataBufferTest, FailedReadsLeaveCursorUnchanged)
{
    DataBuffer buffer{std::vector<std::uint8_t>{1, 2, 3}};
    EXPECT_EQ(buffer.read_uint8(), 1);
    EXPECT_THROW(buffer.read_uint32(), std::out_of_range);
    EXPECT_THROW(buffer.read_bytes((std::numeric_limits<std::size_t>::max)()), std::out_of_range);
    EXPECT_EQ(buffer.read_position(), 1u);
    EXPECT_EQ(buffer.read_uint16(), 0x0203);
    EXPECT_THROW(buffer.read_int8(), std::out_of_range);
    EXPECT_EQ(buffer.read_position(), 3u);
}

TEST(DataBufferTest, AppendsAndReadsRawBytesIncludingZeroes)
{
    const std::array<std::uint8_t, 4> bytes{0, 0xff, 0, 0x42};
    DataBuffer buffer{std::span<const std::uint8_t>{bytes}};
    buffer.write_bytes(bytes);
    EXPECT_THAT(buffer.read_bytes(4), ElementsAre(0, 0xff, 0, 0x42));
    EXPECT_THAT(buffer.read_bytes(4), ElementsAre(0, 0xff, 0, 0x42));
    EXPECT_TRUE(buffer.read_bytes(0).empty());
    EXPECT_EQ(buffer.remaining(), 0u);
}

TEST(DataBufferTest, CanAppendAViewOfItsOwnBytes)
{
    DataBuffer buffer{std::vector<std::uint8_t>{1, 2, 3}};
    buffer.write_bytes(buffer.bytes());
    buffer.write_bytes(buffer.bytes().subspan(1, 2));
    EXPECT_THAT(buffer.bytes(), ElementsAre(1, 2, 3, 1, 2, 3, 2, 3));
}

TEST(DataBufferTest, RewindAndClearResetTheReadCursor)
{
    DataBuffer buffer;
    buffer.reserve(32);
    buffer.write_uint16(0x1234);
    EXPECT_EQ(buffer.read_uint16(), 0x1234);
    buffer.rewind();
    EXPECT_EQ(buffer.read_uint16(), 0x1234);
    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.read_position(), 0u);
    EXPECT_EQ(buffer.remaining(), 0u);
    EXPECT_THROW(buffer.read_uint8(), std::out_of_range);
    buffer.write_uint8(42);
    EXPECT_EQ(buffer.read_uint8(), 42);
}

TEST(DataBufferTest, CopiesPreserveCursorAndOwnTheirStorage)
{
    DataBuffer original{std::vector<std::uint8_t>{1, 2, 3}};
    EXPECT_EQ(original.read_uint8(), 1);
    DataBuffer copy = original;
    DataBuffer assigned;
    assigned = original;
    original.clear();
    EXPECT_EQ(copy.read_uint8(), 2);
    EXPECT_EQ(assigned.read_uint8(), 2);
    EXPECT_THAT(copy.bytes(), ElementsAre(1, 2, 3));
}

TEST(DataBufferTest, MovesPreserveCursorAndLeaveSourceReusable)
{
    DataBuffer source{std::vector<std::uint8_t>{1, 2, 3}};
    EXPECT_EQ(source.read_uint8(), 1);
    DataBuffer moved{std::move(source)};
    EXPECT_EQ(moved.read_position(), 1u);
    EXPECT_EQ(source.read_position(), 0u);
    source.write_uint8(7);
    EXPECT_EQ(source.read_uint8(), 7);
    DataBuffer assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.read_uint8(), 2);
    EXPECT_EQ(moved.read_position(), 0u);
    moved.write_uint8(9);
    EXPECT_EQ(moved.read_uint8(), 9);
}

TEST(DataBufferTest, MessageEqualityIgnoresReadPosition)
{
    DataBuffer first{std::vector<std::uint8_t>{1, 2}};
    DataBuffer second = first;
    EXPECT_EQ(second.read_uint8(), 1);
    EXPECT_EQ(first, second);
    second.write_uint8(3);
    EXPECT_NE(first, second);
}

TEST(DataBufferTest, EmptyByteViewsAreValidAndDoNotChangeCursor)
{
    DataBuffer buffer{std::span<const std::uint8_t>{}};
    buffer.write_bytes({});
    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(buffer.read_bytes(0).empty());
    EXPECT_THROW(buffer.read_uint16(), std::out_of_range);
    EXPECT_EQ(buffer.read_position(), 0u);
}

TEST(DataBufferTest, InvalidByteOrderDoesNotConsumeOrAppendBytes)
{
    DataBuffer buffer{std::vector<std::uint8_t>{1, 2}};
    const auto invalid = static_cast<ByteOrder>(99);
    EXPECT_THROW(buffer.write_uint16(42, invalid), std::invalid_argument);
    EXPECT_THROW(buffer.read_uint16(invalid), std::invalid_argument);
    EXPECT_THAT(buffer.bytes(), ElementsAre(1, 2));
    EXPECT_EQ(buffer.read_position(), 0u);
}

TEST(DataBufferTest, WritesFloat32AsKnownIeee754BytesInBothOrders)
{
    DataBuffer buffer;
    buffer.write_float32(1.0F);
    buffer.write_float32(-2.5F, ByteOrder::little_endian);
    EXPECT_THAT(buffer.bytes(), ElementsAre(0x3f, 0x80, 0, 0, 0, 0, 0x20, 0xc0));
}

TEST(DataBufferTest, ReadsKnownFloat32BytesAndPreservesBoundsGuarantee)
{
    DataBuffer buffer{std::vector<std::uint8_t>{0x3f, 0x80, 0, 0, 0, 0, 0x20, 0xc0}};
    EXPECT_EQ(buffer.read_float32(), 1.0F);
    EXPECT_EQ(buffer.read_float32(ByteOrder::little_endian), -2.5F);
    EXPECT_THROW(buffer.read_float32(), std::out_of_range);
    EXPECT_EQ(buffer.read_position(), 8u);
}

TEST(DataBufferTest, Float32PreservesSpecialValueBitPatterns)
{
    for (const auto bits : {0u, 0x80000000u, 0x7f800000u, 0x7fc12345u})
    {
        DataBuffer buffer;
        buffer.write_float32(std::bit_cast<float>(bits));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(buffer.read_float32()), bits);
    }
}
