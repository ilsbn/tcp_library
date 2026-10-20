#include "message_stream_decoder.hpp"
#include <boost/endian/conversion.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace tcp_helper::detail
{
namespace
{
constexpr std::array<std::uint8_t, 8> magic{'T', 'C', 'P', 'M', 'S', 'G', '0', '1'};
constexpr std::size_t overhead = message_header_size + message_checksum_size;

template <class T> T load(const std::uint8_t* bytes)
{
    return boost::endian::endian_load<T, sizeof(T), boost::endian::order::big>(bytes);
}
}

MessageStreamDecoder::MessageStreamDecoder(std::size_t max_message_size, std::shared_ptr<IMessageErrorLogger> logger,
                                           std::uint64_t connection_id)
    : max_message_size_(max_message_size), logger_(logger ? std::move(logger) : default_message_error_logger()),
      connection_id_(connection_id)
{
    if (max_message_size == 0 || max_message_size > (std::numeric_limits<std::uint32_t>::max)() ||
        max_message_size > (std::numeric_limits<std::size_t>::max)() - read_chunk_size - overhead - 4)
    {
        throw std::invalid_argument("Invalid stream decoder message limit");
    }
}
void MessageStreamDecoder::append(std::span<const std::uint8_t> bytes)
{
    const auto limit = (std::max)(max_message_size_, overhead) + 4 + read_chunk_size;
    if (bytes.size() > read_chunk_size || bytes.size() > limit - buffer_.size())
    {
        throw std::length_error("Drain the stream decoder between bounded read chunks");
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void MessageStreamDecoder::consume(std::size_t count)
{
    if (count == 0)
    {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
    stream_offset_ += count;
    for (auto candidate = rejected_candidates_.begin(); candidate != rejected_candidates_.end();)
    {
        if (*candidate < stream_offset_)
        {
            candidate = rejected_candidates_.erase(candidate);
        }
        else
        {
            ++candidate;
        }
    }
}

void MessageStreamDecoder::report(MessageErrorDetails error, std::size_t start, std::size_t received_size)
{
    error.connection_id = connection_id_;
    error.stream_offset = stream_offset_ + start;
    error.received_size = received_size;
    logger_->log(error);
}

std::optional<DecodedMessage> MessageStreamDecoder::next_message()
{
    if (recovering_)
    {
        return recover();
    }
    if (buffer_.size() < 4)
    {
        return std::nullopt;
    }
    const auto length = load<std::uint32_t>(buffer_.data());
    expected_size_ = static_cast<std::size_t>(length) + 4;
    if (length < overhead || length > max_message_size_)
    {
        report({MessageErrorCode::invalid_frame_length, 0, "Frame length is outside the message limits",
                length < overhead ? overhead : max_message_size_, length},
               0, buffer_.size());
        recovering_ = true;
        scan_begin_ = 0;
        return recover();
    }
    const auto available_magic = (std::min)(magic.size(), buffer_.size() - 4);
    for (std::size_t offset = 0; offset < available_magic; ++offset)
    {
        if (buffer_[4 + offset] != magic[offset])
        {
            report({MessageErrorCode::invalid_magic, offset, "Message magic byte does not match", magic[offset],
                    buffer_[4 + offset]},
                   4, buffer_.size() - 4);
            recovering_ = true;
            scan_begin_ = 0;
            return recover();
        }
    }
    if (buffer_.size() < 4 + magic.size() + sizeof(std::uint32_t))
    {
        return std::nullopt;
    }
    const auto type = load<std::uint32_t>(buffer_.data() + 4 + 8);
    if (type != static_cast<std::uint32_t>(MessageType::Heartbeat) &&
        type != static_cast<std::uint32_t>(MessageType::ShoppingCart))
    {
        report({MessageErrorCode::unknown_message_type, 8, "Unknown message type", {}, type}, 4, buffer_.size() - 4);
        recovering_ = true;
        scan_begin_ = 5; // This header cannot be a valid recovery candidate.
        return recover();
    }
    if (buffer_.size() < 4 + message_header_size)
    {
        return std::nullopt;
    }
    const auto payload_size = load<std::uint64_t>(buffer_.data() + 4 + 12);
    if (payload_size > max_message_size_ - overhead)
    {
        report({MessageErrorCode::payload_size_mismatch, 12, "Payload size exceeds the configured message limit",
                max_message_size_ - overhead, payload_size},
               4, buffer_.size() - 4);
        recovering_ = true;
        scan_begin_ = 5;
        return recover();
    }
    const auto encoded_size = overhead + static_cast<std::size_t>(payload_size);
    if (length != encoded_size)
    {
        report({MessageErrorCode::frame_length_mismatch, 0, "Frame length disagrees with the message header",
                encoded_size, length},
               0, buffer_.size());
        recovering_ = true;
        scan_begin_ = 4; // The message itself may still have a valid header, payload, and CRC.
        return recover();
    }
    if (buffer_.size() < 4 + encoded_size)
    {
        return std::nullopt;
    }
    try
    {
        auto message = decode_message(DataBuffer{std::span<const std::uint8_t>{buffer_}.subspan(4, encoded_size)});
        consume(4 + encoded_size);
        expected_size_ = 4;
        return message;
    }
    catch (const MessageDecodeError& error)
    {
        report(error.details(), 4, encoded_size);
        recovering_ = true;
        scan_begin_ = 5; // Do not report the same rejected candidate again.
        return recover();
    }
}

std::optional<DecodedMessage> MessageStreamDecoder::recover()
{
    auto position = scan_begin_;
    std::optional<std::size_t> pending;
    std::size_t pending_size = magic.size();
    while (position < buffer_.size())
    {
        const auto found = std::search(buffer_.begin() + static_cast<std::ptrdiff_t>(position), buffer_.end(),
                                       magic.begin(), magic.end());
        if (found == buffer_.end())
        {
            break;
        }
        position = static_cast<std::size_t>(found - buffer_.begin());
        const auto available = buffer_.size() - position;
        if (available < message_header_size)
        {
            if (!pending)
            {
                pending = position;
                pending_size = message_header_size;
            }
            break;
        }
        const auto type = load<std::uint32_t>(buffer_.data() + position + 8);
        const auto payload_size = load<std::uint64_t>(buffer_.data() + position + 12);
        if (type > static_cast<std::uint32_t>(MessageType::ShoppingCart) || max_message_size_ < overhead ||
            payload_size > max_message_size_ - overhead)
        {
            ++position;
            continue;
        }
        const auto encoded_size = overhead + static_cast<std::size_t>(payload_size);
        const auto candidate_offset = stream_offset_ + position;
        if (rejected_candidates_.contains(candidate_offset))
        {
            ++position;
            continue;
        }
        if (available < encoded_size)
        {
            if (!pending)
            {
                pending = position;
                pending_size = encoded_size;
            }
            ++position;
            continue; // A later complete candidate may follow a damaged/incomplete header.
        }
        try
        {
            auto message =
                decode_message(DataBuffer{std::span<const std::uint8_t>{buffer_}.subspan(position, encoded_size)});
            consume(position + encoded_size);
            recovering_ = false;
            scan_begin_ = 0;
            expected_size_ = 4;
            return message;
        }
        catch (const MessageDecodeError& error)
        {
            rejected_candidates_.insert(candidate_offset);
            report(error.details(), position, encoded_size);
        }
        ++position;
    }
    if (pending)
    {
        consume(*pending);
        expected_size_ = pending_size;
    }
    else
    {
        // Keep a possible partial magic number across socket reads.
        const auto keep = (std::min)(buffer_.size(), magic.size() - 1);
        consume(buffer_.size() - keep);
        expected_size_ = magic.size();
    }
    scan_begin_ = 0;
    return std::nullopt;
}

std::vector<DecodedMessage> MessageStreamDecoder::finish()
{
    std::vector<DecodedMessage> recovered;
    while (auto message = next_message())
    {
        recovered.push_back(std::move(*message));
    }
    if (!buffer_.empty())
    {
        report({MessageErrorCode::truncated_frame, buffer_.size(), "Stream ended with an incomplete frame",
                expected_size_, buffer_.size()},
               0, buffer_.size());
        recovering_ = true;
        scan_begin_ = 0;
        while (auto message = recover())
        {
            recovered.push_back(std::move(*message));
            recovering_ = true;
            scan_begin_ = 0;
        }
    }
    consume(buffer_.size());
    rejected_candidates_.clear();
    recovering_ = false;
    scan_begin_ = 0;
    expected_size_ = 4;
    return recovered;
}
} // namespace tcp_helper::detail
