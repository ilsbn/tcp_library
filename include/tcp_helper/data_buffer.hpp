#pragma once

#include <tcp_helper/export.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tcp_helper
{

enum class ByteOrder
{
    big_endian,
    little_endian
};

// Owns encoded bytes. Writes append; successful reads advance an independent cursor.
class DataBuffer final
{
public:
    DataBuffer() = default;
    TCP_HELPER_API explicit DataBuffer(std::span<const std::uint8_t> bytes);
    TCP_HELPER_API explicit DataBuffer(std::vector<std::uint8_t> bytes);
    TCP_HELPER_API ~DataBuffer();
    TCP_HELPER_API DataBuffer(const DataBuffer& other);
    TCP_HELPER_API DataBuffer& operator=(const DataBuffer& other);
    TCP_HELPER_API DataBuffer(DataBuffer&& other) noexcept;
    TCP_HELPER_API DataBuffer& operator=(DataBuffer&& other) noexcept;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept
    {
        return storage_;
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return storage_.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return storage_.empty();
    }
    [[nodiscard]] std::size_t read_position() const noexcept
    {
        return read_position_;
    }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return size() - read_position_;
    }

    TCP_HELPER_API void reserve(std::size_t capacity);
    TCP_HELPER_API void clear() noexcept;
    void rewind() noexcept
    {
        read_position_ = 0;
    }

    TCP_HELPER_API void write_bytes(std::span<const std::uint8_t> bytes);
    // Throws std::out_of_range on a short read, without changing the cursor.
    // The returned view is valid until this buffer is mutated or destroyed.
    TCP_HELPER_API std::span<const std::uint8_t> read_bytes(std::size_t count);

    TCP_HELPER_API void write_uint8(std::uint8_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_int8(std::int8_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_uint16(std::uint16_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_int16(std::int16_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_uint32(std::uint32_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_int32(std::int32_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_uint64(std::uint64_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_int64(std::int64_t value, ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API void write_float32(float value, ByteOrder order = ByteOrder::big_endian);

    // All typed reads have the same bounds/cursor guarantees as read_bytes().
    TCP_HELPER_API std::uint8_t read_uint8(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::int8_t read_int8(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::uint16_t read_uint16(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::int16_t read_int16(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::uint32_t read_uint32(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::int32_t read_int32(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::uint64_t read_uint64(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API std::int64_t read_int64(ByteOrder order = ByteOrder::big_endian);
    TCP_HELPER_API float read_float32(ByteOrder order = ByteOrder::big_endian);

    // Message equality compares bytes, independently of each buffer's read position.
    [[nodiscard]] TCP_HELPER_API bool operator==(const DataBuffer& other) const noexcept;

private:
    template <class T> void write_integer(T value, ByteOrder order);
    template <class T> T read_integer(ByteOrder order);

    std::vector<std::uint8_t> storage_;
    std::size_t read_position_ = 0;
};

} // namespace tcp_helper
