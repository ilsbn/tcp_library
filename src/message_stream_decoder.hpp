#pragma once

#include <tcp_helper/messages.hpp>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <unordered_set>

namespace tcp_helper::detail
{
class MessageStreamDecoder
{
public:
    static constexpr std::size_t read_chunk_size = 4096;
    TCP_HELPER_API MessageStreamDecoder(std::size_t max_message_size, std::shared_ptr<IMessageErrorLogger> logger,
                                        std::uint64_t connection_id = 0);
    TCP_HELPER_API void append(std::span<const std::uint8_t> bytes);
    [[nodiscard]] TCP_HELPER_API std::optional<DecodedMessage> next_message();
    TCP_HELPER_API std::vector<DecodedMessage> finish();
    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return buffer_.size();
    }

private:
    std::optional<DecodedMessage> recover();
    void consume(std::size_t count);
    void report(MessageErrorDetails error, std::size_t start, std::size_t received_size);
    std::vector<std::uint8_t> buffer_;
    std::size_t max_message_size_;
    std::shared_ptr<IMessageErrorLogger> logger_;
    std::uint64_t connection_id_;
    std::uint64_t stream_offset_ = 0;
    bool recovering_ = false;
    std::size_t scan_begin_ = 0;
    std::size_t expected_size_ = 4;
    std::unordered_set<std::uint64_t> rejected_candidates_;
};
} // namespace tcp_helper::detail
