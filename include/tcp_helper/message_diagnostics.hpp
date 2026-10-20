#pragma once

#include <tcp_helper/export.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tcp_helper
{
enum class MessageErrorCode
{
    invalid_message_size,
    checksum_mismatch,
    invalid_magic,
    unknown_message_type,
    payload_size_mismatch,
    truncated_field,
    invalid_login_length,
    invalid_item_count,
    trailing_payload,
    invalid_frame_length,
    frame_length_mismatch,
    truncated_frame
};

struct MessageErrorDetails
{
    MessageErrorCode code = MessageErrorCode::invalid_message_size;
    std::size_t byte_offset = 0; // Field offset relative to the message/frame start.
    std::string description;
    std::optional<std::uint64_t> expected;
    std::optional<std::uint64_t> actual;
    std::size_t received_size = 0; // Receive context is populated by the stream decoder.
    std::uint64_t connection_id = 0;
    std::uint64_t stream_offset = 0;
};

class MessageDecodeError : public std::invalid_argument
{
public:
    TCP_HELPER_API explicit MessageDecodeError(MessageErrorDetails details);
    [[nodiscard]] const MessageErrorDetails& details() const noexcept
    {
        return details_;
    }

private:
    MessageErrorDetails details_;
};

class IMessageErrorLogger
{
public:
    virtual ~IMessageErrorLogger() = default;
    // Invoked on the connection's event thread. Implementations must not throw.
    virtual void log(const MessageErrorDetails& error) noexcept = 0;
};

[[nodiscard]] TCP_HELPER_API std::string_view message_error_name(MessageErrorCode code) noexcept;
// Appends and flushes errors. Throws during setup if the file cannot be opened.
[[nodiscard]] TCP_HELPER_API std::shared_ptr<IMessageErrorLogger> make_file_message_error_logger(std::string path);
[[nodiscard]] TCP_HELPER_API std::shared_ptr<IMessageErrorLogger> default_message_error_logger();
} // namespace tcp_helper
