#include <tcp_helper/message_diagnostics.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cstdio>
#include <utility>

namespace tcp_helper
{
MessageDecodeError::MessageDecodeError(MessageErrorDetails details)
    : std::invalid_argument(details.description), details_(std::move(details))
{
}

namespace
{
void report_log_failure(const std::string& error)
{
    std::fprintf(stderr, "tcp_helper: message error log failure: %s\n", error.c_str());
}

class FileMessageErrorLogger final : public IMessageErrorLogger
{
public:
    explicit FileMessageErrorLogger(const std::string& path)
        : logger_("tcp_helper_errors", std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, false))
    {
        logger_.set_error_handler(&report_log_failure);
        logger_.flush_on(spdlog::level::err);
    }
    void log(const MessageErrorDetails& error) noexcept override
    {
        try
        {
            logger_.error(
                "code={} connection={} stream_offset={} byte_offset={} received_size={} expected={} actual={} description={}",
                message_error_name(error.code), error.connection_id, error.stream_offset, error.byte_offset,
                error.received_size, error.expected ? std::to_string(*error.expected) : "n/a",
                error.actual ? std::to_string(*error.actual) : "n/a", error.description);
        }
        catch (...)
        {
            std::fputs("tcp_helper: could not write message error log\n", stderr);
        }
    }

private:
    spdlog::logger logger_;
};
}

std::string_view message_error_name(MessageErrorCode code) noexcept
{
    switch (code)
    {
    case MessageErrorCode::invalid_message_size:
        return "invalid_message_size";
    case MessageErrorCode::checksum_mismatch:
        return "checksum_mismatch";
    case MessageErrorCode::invalid_magic:
        return "invalid_magic";
    case MessageErrorCode::unknown_message_type:
        return "unknown_message_type";
    case MessageErrorCode::payload_size_mismatch:
        return "payload_size_mismatch";
    case MessageErrorCode::truncated_field:
        return "truncated_field";
    case MessageErrorCode::invalid_login_length:
        return "invalid_login_length";
    case MessageErrorCode::invalid_item_count:
        return "invalid_item_count";
    case MessageErrorCode::trailing_payload:
        return "trailing_payload";
    case MessageErrorCode::invalid_frame_length:
        return "invalid_frame_length";
    case MessageErrorCode::frame_length_mismatch:
        return "frame_length_mismatch";
    case MessageErrorCode::truncated_frame:
        return "truncated_frame";
    }
    return "unknown";
}
std::shared_ptr<IMessageErrorLogger> make_file_message_error_logger(std::string path)
{
    return std::make_shared<FileMessageErrorLogger>(path);
}
std::shared_ptr<IMessageErrorLogger> default_message_error_logger()
{
    static const auto logger = make_file_message_error_logger("logs/tcp_helper_errors.log");
    return logger;
}
} // namespace tcp_helper
