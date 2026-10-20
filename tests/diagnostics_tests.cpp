#include <tcp_helper/messages.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <array>
#include <chrono>

using namespace tcp_helper;

namespace
{
DataBuffer replace_crc(std::vector<std::uint8_t> bytes)
{
    bytes.resize(bytes.size() - message_checksum_size);
    DataBuffer encoded{std::move(bytes)};
    encoded.write_uint32(compute_crc32(encoded.bytes()));
    return encoded;
}

void expect_error(const DataBuffer& bytes, MessageErrorCode code, std::size_t offset)
{
    try
    {
        static_cast<void>(decode_message(bytes));
        FAIL() << "Expected invalid packet";
    }
    catch (const MessageDecodeError& error)
    {
        EXPECT_EQ(error.details().code, code);
        EXPECT_EQ(error.details().byte_offset, offset);
        EXPECT_FALSE(error.details().description.empty());
    }
    catch (const std::invalid_argument&)
    {
        FAIL() << "Expected structured MessageDecodeError";
    }
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

void expect_details(const DataBuffer& bytes, MessageErrorCode code, std::size_t offset, std::uint64_t expected,
                    std::uint64_t actual)
{
    try
    {
        static_cast<void>(decode_message(bytes));
        FAIL() << "Expected structured diagnostic";
    }
    catch (const MessageDecodeError& error)
    {
        EXPECT_EQ(error.details().code, code);
        EXPECT_EQ(error.details().byte_offset, offset);
        EXPECT_EQ(error.details().expected, expected);
        EXPECT_EQ(error.details().actual, actual);
    }
}

std::filesystem::path temporary_path()
{
    return std::filesystem::temp_directory_path() /
           ("tcp_helper_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
}
}

TEST(MessageDiagnostics, ReportsChecksumOffsetAndValues)
{
    const auto valid = encode_message(Heartbeat{1, {"A", 2}});
    auto bytes = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    bytes[20] ^= 1;
    expect_error(DataBuffer{bytes}, MessageErrorCode::checksum_mismatch, 41);
}

TEST(MessageDiagnostics, DistinguishesCrcValidHeaderErrors)
{
    const auto valid = encode_message(Heartbeat{1, {"A", 2}});
    auto bytes = std::vector<std::uint8_t>(valid.bytes().begin(), valid.bytes().end());
    bytes[0] ^= 1;
    expect_error(replace_crc(bytes), MessageErrorCode::invalid_magic, 0);
    bytes.assign(valid.bytes().begin(), valid.bytes().end());
    bytes[11] = 99;
    expect_error(replace_crc(bytes), MessageErrorCode::unknown_message_type, 8);
    bytes.assign(valid.bytes().begin(), valid.bytes().end());
    bytes[19] = 0;
    expect_error(replace_crc(bytes), MessageErrorCode::payload_size_mismatch, 12);
}

TEST(MessageDiagnostics, FileLoggerWritesStructuredDetails)
{
    const auto path = temporary_path();
    auto logger = make_file_message_error_logger(path.string());
    logger->log({MessageErrorCode::checksum_mismatch, 41, "Message checksum mismatch", 123, 456, 45, 7, 900});
    std::ifstream input{path};
    ASSERT_TRUE(input.is_open());
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    EXPECT_THAT(text, testing::HasSubstr("checksum_mismatch"));
    EXPECT_THAT(text, testing::HasSubstr("connection=7"));
    EXPECT_THAT(text, testing::HasSubstr("stream_offset=900"));
    EXPECT_THAT(text, testing::HasSubstr("byte_offset=41"));
    EXPECT_THAT(text, testing::HasSubstr("expected=123"));
    EXPECT_THAT(text, testing::HasSubstr("actual=456"));
    input.close();
    logger.reset();
    std::filesystem::remove(path);
}

TEST(MessageDiagnostics, EveryTooShortSizeReportsRequiredAndActualBytes)
{
    for (std::size_t size = 0; size < message_header_size + message_checksum_size; ++size)
    {
        expect_details(DataBuffer{std::vector<std::uint8_t>(size)}, MessageErrorCode::invalid_message_size, 0, 24,
                       size);
    }
}

TEST(MessageDiagnostics, CrcValidTruncatedFieldsReportTheirExactOffsets)
{
    DataBuffer payload;
    payload.write_bytes(std::array<std::uint8_t, 7>{});
    expect_details(make_packet(MessageType::Heartbeat, payload), MessageErrorCode::truncated_field, 20, 8, 7);
    payload.clear();
    payload.write_uint64(1);
    payload.write_uint32(0);
    expect_details(make_packet(MessageType::Heartbeat, payload), MessageErrorCode::truncated_field, 28, 12, 4);
    payload.clear();
    payload.write_uint32(0);
    payload.write_uint64(2);
    expect_details(make_packet(MessageType::ShoppingCart, payload), MessageErrorCode::truncated_field, 32, 4, 0);
}

TEST(MessageDiagnostics, CrcValidLoginAndItemLengthsHaveDifferentErrorCategories)
{
    DataBuffer heartbeat;
    heartbeat.write_uint64(1);
    heartbeat.write_uint32(0xffffffffu);
    heartbeat.write_uint8('A');
    heartbeat.write_uint64(2);
    expect_details(make_packet(MessageType::Heartbeat, heartbeat), MessageErrorCode::invalid_login_length, 28, 1,
                   0xffffffffu);
    DataBuffer cart;
    cart.write_uint32(0);
    cart.write_uint64(2);
    cart.write_uint32(2);
    cart.write_uint64(3); // An incomplete product must not allocate list nodes.
    expect_details(make_packet(MessageType::ShoppingCart, cart), MessageErrorCode::invalid_item_count, 32, 0, 2);
}

TEST(MessageDiagnostics, CrcValidTrailingPayloadReportsTheFirstExtraByte)
{
    DataBuffer heartbeat;
    heartbeat.write_uint64(1);
    heartbeat.write_uint32(0);
    heartbeat.write_uint64(2);
    heartbeat.write_uint8(99);
    expect_details(make_packet(MessageType::Heartbeat, heartbeat), MessageErrorCode::trailing_payload, 40, 0, 1);
    DataBuffer cart;
    cart.write_uint32(0);
    cart.write_uint64(2);
    cart.write_uint32(0);
    cart.write_uint8(99);
    expect_details(make_packet(MessageType::ShoppingCart, cart), MessageErrorCode::trailing_payload, 36, 0, 1);
}

TEST(MessageDiagnostics, FileLoggerAppendsAndReportsSetupFailure)
{
    const auto path = temporary_path();
    {
        auto logger = make_file_message_error_logger(path.string());
        logger->log({MessageErrorCode::invalid_magic, 0, "First record"});
    }
    {
        auto logger = make_file_message_error_logger(path.string());
        logger->log({MessageErrorCode::invalid_item_count, 32, "Second record"});
    }
    std::ifstream input{path};
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    EXPECT_THAT(text, testing::HasSubstr("First record"));
    EXPECT_THAT(text, testing::HasSubstr("Second record"));
    input.close();
    std::filesystem::remove(path);
    const auto directory = temporary_path();
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    EXPECT_THROW(static_cast<void>(make_file_message_error_logger(directory.string())), std::exception);
    std::filesystem::remove(directory);
}
