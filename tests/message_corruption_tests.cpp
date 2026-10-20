#include <tcp_helper/messages.hpp>
#include <gtest/gtest.h>
#include <array>
#include <vector>

using namespace tcp_helper;

namespace
{
std::array<DataBuffer, 2> packets()
{
    return {encode_message(Heartbeat{1, {"A", 2}}), encode_message(ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}})};
}

void rejects(const std::vector<std::uint8_t>& bytes)
{
    EXPECT_THROW(static_cast<void>(decode_message(DataBuffer{bytes})), MessageDecodeError);
}
}

TEST(MessageCorruption, EveryReplacementByteAtEveryPositionInBothMessagesIsDetected)
{
    std::size_t mutations = 0;
    for (const auto& packet : packets())
    {
        std::vector<std::uint8_t> bytes(packet.bytes().begin(), packet.bytes().end());
        for (std::size_t offset = 0; offset < bytes.size(); ++offset)
        {
            const auto original = bytes[offset];
            for (unsigned value = 0; value <= 255; ++value)
            {
                if (value == original)
                {
                    continue;
                }
                bytes[offset] = static_cast<std::uint8_t>(value);
                try
                {
                    static_cast<void>(decode_message(DataBuffer{bytes}));
                    FAIL() << "Accepted offset=" << offset << " replacement=" << value;
                }
                catch (const MessageDecodeError& error)
                {
                    ASSERT_EQ(error.details().code, MessageErrorCode::checksum_mismatch);
                    ASSERT_EQ(error.details().byte_offset, bytes.size() - 4);
                    ASSERT_TRUE(error.details().expected);
                    ASSERT_TRUE(error.details().actual);
                    ASSERT_NE(error.details().expected, error.details().actual);
                }
                ++mutations;
            }
            bytes[offset] = original;
        }
    }
    EXPECT_EQ(mutations, (45 + 65) * 255u);
}

TEST(MessageCorruption, ContiguousBitBurstsUpTo32BitsAreDetectedAcrossAllFields)
{
    constexpr std::array<std::size_t, 10> lengths{2, 3, 7, 8, 9, 15, 16, 17, 31, 32};
    for (const auto& packet : packets())
    {
        const std::vector<std::uint8_t> original(packet.bytes().begin(), packet.bytes().end());
        for (const auto length : lengths)
        {
            for (std::size_t start = 0; start + length <= original.size() * 8; ++start)
            {
                auto bytes = original;
                for (std::size_t bit = start; bit < start + length; ++bit)
                {
                    bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
                }
                rejects(bytes);
            }
        }
    }
}

TEST(MessageCorruption, EveryTruncationDeletionInsertionAndAdjacentSwapIsDetected)
{
    for (const auto& packet : packets())
    {
        const std::vector<std::uint8_t> original(packet.bytes().begin(), packet.bytes().end());
        for (std::size_t size = 0; size < original.size(); ++size)
        {
            rejects(std::vector<std::uint8_t>(original.begin(), original.begin() + size));
        }
        for (std::size_t offset = 0; offset < original.size(); ++offset)
        {
            auto bytes = original;
            bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            rejects(bytes);
            if (offset + 1 < original.size() && original[offset] != original[offset + 1])
            {
                bytes = original;
                std::swap(bytes[offset], bytes[offset + 1]);
                rejects(bytes);
            }
        }
        for (std::size_t offset = 0; offset <= original.size(); ++offset)
        {
            for (const auto value : std::array<std::uint8_t, 3>{0, 0x54, 0xff})
            {
                auto bytes = original;
                bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), value);
                rejects(bytes);
            }
        }
    }
}
