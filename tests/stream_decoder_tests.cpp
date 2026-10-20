#include "message_stream_decoder.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <vector>

using namespace tcp_helper;
using tcp_helper::detail::MessageStreamDecoder;

namespace
{
class CapturingLogger final : public IMessageErrorLogger
{
public:
    void log(const MessageErrorDetails& error) noexcept override
    {
        errors.push_back(error);
    }
    std::vector<MessageErrorDetails> errors;
};

DataBuffer frame(const DataBuffer& packet)
{
    DataBuffer bytes;
    bytes.write_uint32(static_cast<std::uint32_t>(packet.size()));
    bytes.write_bytes(packet.bytes());
    return bytes;
}

std::vector<MessageBody> feed(MessageStreamDecoder& decoder, const DataBuffer& bytes, std::size_t chunk_size)
{
    std::vector<MessageBody> messages;
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunk_size)
    {
        decoder.append(bytes.bytes().subspan(offset, (std::min)(chunk_size, bytes.size() - offset)));
        while (auto message = decoder.next_message())
        {
            messages.push_back(std::move(message->m_body));
        }
    }
    return messages;
}

class MockErrorLogger : public IMessageErrorLogger
{
public:
    MOCK_METHOD(void, log, (const MessageErrorDetails&), (noexcept, override));
};
}

TEST(MessageStream, RecoversAfterBadChecksumWithoutLosingNextMessage)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{1024, logger, 17};
    const Heartbeat expected{1, {"A", 2}};
    const auto valid = encode_message(expected);
    auto corrupt = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    corrupt.back() ^= 1;
    auto stream = frame(DataBuffer{corrupt});
    stream.write_bytes(frame(valid).bytes());
    decoder.append(stream.bytes());
    const auto decoded = decoder.next_message();
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<Heartbeat>(decoded->m_body), expected);
    ASSERT_EQ(logger->errors.size(), 1);
    EXPECT_EQ(logger->errors.front().code, MessageErrorCode::checksum_mismatch);
    EXPECT_EQ(logger->errors.front().connection_id, 17);
    EXPECT_FALSE(decoder.next_message());
}

TEST(MessageStream, RecoversMessageWithCorruptedOuterLength)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{1024, logger};
    DataBuffer stream;
    stream.write_uint32(0xffffffffu);
    stream.write_bytes(encode_message(Heartbeat{1, {"A", 2}}).bytes());
    decoder.append(stream.bytes());
    ASSERT_TRUE(decoder.next_message());
    ASSERT_FALSE(logger->errors.empty());
    EXPECT_EQ(logger->errors.front().code, MessageErrorCode::invalid_frame_length);
}

TEST(MessageStream, InvalidMagicStartsRecoveryBeforeTheRestOfTheHeaderArrives)
{
    constexpr std::array<std::uint8_t, 8> expected_magic{'T', 'C', 'P', 'M', 'S', 'G', '0', '1'};
    for (std::size_t offset = 0; offset < expected_magic.size(); ++offset)
    {
        auto logger = std::make_shared<CapturingLogger>();
        MessageStreamDecoder decoder{1024, logger};
        DataBuffer partial;
        partial.write_uint32(924);
        partial.write_bytes(std::span<const std::uint8_t>{expected_magic}.first(offset));
        partial.write_uint8(0xff);
        decoder.append(partial.bytes());
        EXPECT_FALSE(decoder.next_message());
        ASSERT_EQ(logger->errors.size(), 1) << "magic offset=" << offset;
        EXPECT_EQ(logger->errors.front().code, MessageErrorCode::invalid_magic);
        EXPECT_EQ(logger->errors.front().byte_offset, offset);
        EXPECT_EQ(logger->errors.front().expected, expected_magic[offset]);
        EXPECT_EQ(logger->errors.front().actual, 0xff);
        const auto messages = feed(decoder, frame(encode_message(Heartbeat{7, {"A", 2}})), 1);
        ASSERT_EQ(messages.size(), 1);
        EXPECT_EQ(std::get<Heartbeat>(messages.front()).m_sequence_number, 7);
        EXPECT_EQ(logger->errors.size(), 1);
    }
}

TEST(MessageStream, UnsupportedTypeStartsRecoveryBeforePayloadSizeOrBodyArrives)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{1024, logger, 19};
    DataBuffer partial;
    partial.write_uint32(924);
    partial.write_uint64(message_magic_number);
    partial.write_uint32(99);
    // Neither the payload-size field nor the advertised body will be sent.
    decoder.append(partial.bytes());
    EXPECT_FALSE(decoder.next_message());
    ASSERT_EQ(logger->errors.size(), 1);
    EXPECT_EQ(logger->errors.front().code, MessageErrorCode::unknown_message_type);
    EXPECT_EQ(logger->errors.front().byte_offset, 8);
    EXPECT_EQ(logger->errors.front().actual, 99);
    EXPECT_EQ(logger->errors.front().connection_id, 19);
    EXPECT_EQ(logger->errors.front().stream_offset, 4);
    const ShoppingCart expected{{"A", 2}, {{3, 1.5F}}};
    const auto messages = feed(decoder, frame(encode_message(expected)), 1);
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(std::get<ShoppingCart>(messages.front()), expected);
    EXPECT_EQ(logger->errors.size(), 1);
}

TEST(MessageStream, EveryFrameLengthByteReplacementRecoversBothMessageTypes)
{
    const std::array<MessageBody, 2> bodies{Heartbeat{1, {"A", 2}}, ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}}};
    for (const auto& body : bodies)
    {
        const auto valid = frame(encode_message(body));
        for (std::size_t offset = 0; offset < 4; ++offset)
        {
            for (unsigned value = 0; value <= 255; ++value)
            {
                if (value == valid.bytes()[offset])
                {
                    continue;
                }
                auto logger = std::make_shared<CapturingLogger>();
                MessageStreamDecoder decoder{1024, logger};
                auto bytes = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
                bytes[offset] = static_cast<std::uint8_t>(value);
                const auto messages = feed(decoder, DataBuffer{bytes}, 7);
                ASSERT_EQ(messages.size(), 1);
                ASSERT_EQ(messages.front(), body);
                ASSERT_EQ(logger->errors.size(), 1);
                const auto code = logger->errors.front().code;
                ASSERT_TRUE(code == MessageErrorCode::invalid_frame_length ||
                            code == MessageErrorCode::frame_length_mismatch);
            }
        }
    }
}

TEST(MessageStream, FragmentedMagicAndGarbageRecoverAtEveryReadSize)
{
    const Heartbeat expected{77, {"alice", 42}};
    DataBuffer stream{std::vector<std::uint8_t>(19, 0xff)};
    stream.write_bytes(frame(encode_message(expected)).bytes());
    stream.write_bytes(frame(encode_message(ShoppingCart{})).bytes());
    for (std::size_t chunk_size = 1; chunk_size <= stream.size(); ++chunk_size)
    {
        auto logger = std::make_shared<CapturingLogger>();
        MessageStreamDecoder decoder{1024, logger};
        const auto messages = feed(decoder, stream, chunk_size);
        ASSERT_EQ(messages.size(), 2);
        EXPECT_EQ(std::get<Heartbeat>(messages[0]), expected);
        EXPECT_EQ(std::get<ShoppingCart>(messages[1]), ShoppingCart{});
        EXPECT_FALSE(logger->errors.empty());
        EXPECT_EQ(decoder.buffered_size(), 0);
    }
}

TEST(MessageStream, SkipsIncompleteMisleadingHeaderAndUnverifiedMagicCandidates)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{1024, logger};
    DataBuffer stream;
    stream.write_uint32(0xffffffffu);
    stream.write_uint64(message_magic_number);
    stream.write_uint32(0);
    stream.write_uint64(900); // Incomplete; must not prevent finding the next complete candidate.
    stream.write_uint64(message_magic_number);
    stream.write_uint32(99); // A magic number alone does not make a candidate valid.
    stream.write_uint64(0);
    stream.write_uint64(message_magic_number);
    stream.write_uint32(0);
    stream.write_uint64(0);
    stream.write_uint32(0); // Wrong CRC.
    const Heartbeat expected{9, {"A", 2}};
    stream.write_bytes(frame(encode_message(expected)).bytes());
    const auto messages = feed(decoder, stream, 3);
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(std::get<Heartbeat>(messages.front()), expected);
    EXPECT_FALSE(logger->errors.empty());
}

TEST(MessageStream, MagicInsideValidPayloadDoesNotSplitFragmentedMessage)
{
    DataBuffer fake_header;
    fake_header.write_uint64(message_magic_number);
    fake_header.write_uint32(99);
    fake_header.write_uint64(0xffffffffffffffffull);
    std::string login(200, 'x');
    login.append(reinterpret_cast<const char*>(fake_header.bytes().data()), fake_header.size());
    login += "TCPMSG01";
    const Heartbeat expected{1, {login, 2}};
    const auto stream = frame(encode_message(expected));
    for (const std::size_t chunk_size : {1u, 3u, 7u, 128u})
    {
        auto logger = std::make_shared<CapturingLogger>();
        MessageStreamDecoder decoder{1024, logger};
        const auto messages = feed(decoder, stream, chunk_size);
        ASSERT_EQ(messages.size(), 1);
        EXPECT_EQ(std::get<Heartbeat>(messages.front()), expected);
        EXPECT_TRUE(logger->errors.empty());
    }
}

TEST(MessageStream, RecoversAfterEveryPacketTruncationAndSingleByteInsertionOrDeletion)
{
    const std::array<MessageBody, 2> bodies{Heartbeat{1, {"A", 2}}, ShoppingCart{{"A", 2}, {{3, 1.5F}, {4, 2.25F}}}};
    const ShoppingCart marker{{"marker", 99}, {{5, 7.5F}, {6, 8.5F}}};
    const auto next = frame(encode_message(marker));
    for (const auto& body : bodies)
    {
        const auto packet = encode_message(body);
        const std::vector<std::uint8_t> original(packet.bytes().begin(), packet.bytes().end());
        for (unsigned operation = 0; operation < 3; ++operation)
        {
            for (std::size_t offset = 0; offset < original.size(); ++offset)
            {
                auto bytes = original;
                if (operation == 0)
                {
                    bytes.resize(offset);
                }
                else if (operation == 1)
                {
                    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                }
                else
                {
                    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 0xff);
                }
                DataBuffer stream;
                stream.write_uint32(static_cast<std::uint32_t>(original.size()));
                stream.write_bytes(bytes);
                stream.write_bytes(next.bytes());
                auto logger = std::make_shared<CapturingLogger>();
                MessageStreamDecoder decoder{1024, logger};
                const auto messages = feed(decoder, stream, 5);
                const auto expected_count = operation == 2 && offset == 0 ? 2u : 1u;
                ASSERT_EQ(messages.size(), expected_count) << "operation=" << operation << " offset=" << offset;
                if (expected_count == 2)
                {
                    EXPECT_EQ(messages.front(), body); // An inserted byte before the magic leaves the message intact.
                }
                EXPECT_EQ(std::get<ShoppingCart>(messages.back()), marker);
                EXPECT_FALSE(logger->errors.empty());
            }
        }
    }
}

TEST(MessageStream, LongGarbageStreamUsesBoundedBufferingAndThenRecovers)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{128, logger};
    const std::array<std::uint8_t, MessageStreamDecoder::read_chunk_size> garbage = []
    {
        std::array<std::uint8_t, MessageStreamDecoder::read_chunk_size> bytes{};
        bytes.fill(0xff);
        return bytes;
    }();
    for (unsigned chunk = 0; chunk < 512; ++chunk)
    {
        decoder.append(garbage);
        EXPECT_LE(decoder.buffered_size(), 128 + 4 + MessageStreamDecoder::read_chunk_size);
        EXPECT_FALSE(decoder.next_message());
        EXPECT_LE(decoder.buffered_size(), 7);
    }
    const auto messages = feed(decoder, frame(encode_message(Heartbeat{1, {"A", 2}})), 1);
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(decoder.buffered_size(), 0);
}

TEST(MessageStream, EndOfStreamReportsTruncatedFrameWithSizes)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{1024, logger, 5};
    const auto valid = frame(encode_message(Heartbeat{1, {"A", 2}}));
    decoder.append(valid.bytes().first(17));
    EXPECT_FALSE(decoder.next_message());
    decoder.finish();
    ASSERT_EQ(logger->errors.size(), 1);
    const auto& error = logger->errors.front();
    EXPECT_EQ(error.code, MessageErrorCode::truncated_frame);
    EXPECT_EQ(error.byte_offset, 17);
    EXPECT_EQ(error.expected, 49);
    EXPECT_EQ(error.actual, 17);
    EXPECT_EQ(error.connection_id, 5);
    EXPECT_EQ(decoder.buffered_size(), 0);
}

TEST(MessageStream, LoggerInterfaceCanBeReplacedByGoogleMock)
{
    auto logger = std::make_shared<testing::StrictMock<MockErrorLogger>>();
    MessageErrorDetails observed;
    EXPECT_CALL(*logger, log(testing::Field(&MessageErrorDetails::code, MessageErrorCode::checksum_mismatch)))
        .WillOnce(testing::SaveArg<0>(&observed));
    MessageStreamDecoder decoder{1024, logger, 123};
    const auto valid = encode_message(Heartbeat{1, {"A", 2}});
    auto bytes = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    bytes.back() ^= 1;
    auto stream = frame(DataBuffer{bytes});
    stream.write_bytes(frame(valid).bytes());
    EXPECT_EQ(feed(decoder, stream, 3).size(), 1);
    EXPECT_EQ(observed.byte_offset, 41);
    EXPECT_EQ(observed.connection_id, 123);
    EXPECT_EQ(observed.stream_offset, 4);
}

TEST(MessageStream, RejectedCandidateIsLoggedOnlyOnceAcrossSubsequentReads)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{1024, logger};
    DataBuffer stream;
    stream.write_uint32(0xffffffffu);
    stream.write_uint64(message_magic_number);
    stream.write_uint32(0);
    stream.write_uint64(900);
    stream.write_uint64(message_magic_number);
    stream.write_uint32(0);
    stream.write_uint64(0);
    stream.write_uint32(0);
    decoder.append(stream.bytes());
    EXPECT_FALSE(decoder.next_message());
    const std::array<std::uint8_t, 1> extra{'x'};
    for (unsigned chunk = 0; chunk < 10; ++chunk)
    {
        decoder.append(extra);
        EXPECT_FALSE(decoder.next_message());
    }
    const auto crc_errors = std::count_if(logger->errors.begin(), logger->errors.end(),
                                          [](const MessageErrorDetails& error)
                                          {
                                              return error.code == MessageErrorCode::checksum_mismatch;
                                          });
    EXPECT_EQ(crc_errors, 1);
}

TEST(MessageStream, EndOfStreamRecoversSmallMessageBehindLargeTruncatedFrame)
{
    auto logger = std::make_shared<CapturingLogger>();
    MessageStreamDecoder decoder{2048, logger};
    const auto large = frame(encode_message(ShoppingCart{{std::string(1000, 'x'), 42}, {}}));
    DataBuffer stream{large.bytes().first(24)};
    const Heartbeat expected{7, {"A", 2}};
    stream.write_bytes(frame(encode_message(expected)).bytes());
    decoder.append(stream.bytes());
    EXPECT_FALSE(decoder.next_message());
    const auto recovered = decoder.finish();
    ASSERT_EQ(recovered.size(), 1);
    EXPECT_EQ(std::get<Heartbeat>(recovered.front().m_body), expected);
    EXPECT_EQ(decoder.buffered_size(), 0);
    ASSERT_FALSE(logger->errors.empty());
    EXPECT_EQ(logger->errors.front().code, MessageErrorCode::truncated_frame);
}
