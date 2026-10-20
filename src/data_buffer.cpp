#include <tcp_helper/data_buffer.hpp>
#include <boost/endian/conversion.hpp>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tcp_helper
{

namespace
{
void validate_order(ByteOrder order)
{
    if (order != ByteOrder::big_endian && order != ByteOrder::little_endian)
    {
        throw std::invalid_argument("Invalid byte order");
    }
}
} // namespace

DataBuffer::DataBuffer(std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty())
    {
        storage_.assign(bytes.begin(), bytes.end());
    }
}
DataBuffer::DataBuffer(std::vector<std::uint8_t> bytes) : storage_(std::move(bytes))
{
}
DataBuffer::~DataBuffer() = default;
DataBuffer::DataBuffer(const DataBuffer&) = default;
DataBuffer& DataBuffer::operator=(const DataBuffer&) = default;
DataBuffer::DataBuffer(DataBuffer&& other) noexcept
    : storage_(std::move(other.storage_)), read_position_(std::exchange(other.read_position_, 0))
{
    other.storage_.clear();
}
DataBuffer& DataBuffer::operator=(DataBuffer&& other) noexcept
{
    if (this != &other)
    {
        storage_ = std::move(other.storage_);
        read_position_ = std::exchange(other.read_position_, 0);
        other.storage_.clear();
    }
    return *this;
}

void DataBuffer::reserve(std::size_t capacity)
{
    storage_.reserve(capacity);
}
void DataBuffer::clear() noexcept
{
    storage_.clear();
    read_position_ = 0;
}

void DataBuffer::write_bytes(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
    {
        return;
    }
    // A view into this buffer would be invalidated if insert reallocates storage.
    const std::less<const std::uint8_t*> before;
    if (!storage_.empty() && !before(bytes.data(), storage_.data()) &&
        before(bytes.data(), storage_.data() + storage_.size()))
    {
        const std::vector<std::uint8_t> copy(bytes.begin(), bytes.end());
        storage_.insert(storage_.end(), copy.begin(), copy.end());
    }
    else
    {
        storage_.insert(storage_.end(), bytes.begin(), bytes.end());
    }
}

std::span<const std::uint8_t> DataBuffer::read_bytes(std::size_t count)
{
    if (count > remaining())
    {
        throw std::out_of_range("Not enough bytes in DataBuffer");
    }
    if (count == 0)
    {
        return {};
    }
    const auto result = bytes().subspan(read_position_, count);
    read_position_ += count;
    return result;
}

template <class T> void DataBuffer::write_integer(T value, ByteOrder order)
{
    validate_order(order);
    std::array<std::uint8_t, sizeof(T)> encoded{};
    if (order == ByteOrder::big_endian)
    {
        boost::endian::endian_store<T, sizeof(T), boost::endian::order::big>(encoded.data(), value);
    }
    else
    {
        boost::endian::endian_store<T, sizeof(T), boost::endian::order::little>(encoded.data(), value);
    }
    write_bytes(encoded);
}

template <class T> T DataBuffer::read_integer(ByteOrder order)
{
    validate_order(order);
    const auto encoded = read_bytes(sizeof(T));
    if (order == ByteOrder::big_endian)
    {
        return boost::endian::endian_load<T, sizeof(T), boost::endian::order::big>(encoded.data());
    }
    return boost::endian::endian_load<T, sizeof(T), boost::endian::order::little>(encoded.data());
}

void DataBuffer::write_uint8(std::uint8_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_int8(std::int8_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_uint16(std::uint16_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_int16(std::int16_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_uint32(std::uint32_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_int32(std::int32_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_uint64(std::uint64_t value, ByteOrder order)
{
    write_integer(value, order);
}
void DataBuffer::write_int64(std::int64_t value, ByteOrder order)
{
    write_integer(value, order);
}
std::uint8_t DataBuffer::read_uint8(ByteOrder order)
{
    return read_integer<std::uint8_t>(order);
}
std::int8_t DataBuffer::read_int8(ByteOrder order)
{
    return read_integer<std::int8_t>(order);
}
std::uint16_t DataBuffer::read_uint16(ByteOrder order)
{
    return read_integer<std::uint16_t>(order);
}
std::int16_t DataBuffer::read_int16(ByteOrder order)
{
    return read_integer<std::int16_t>(order);
}
std::uint32_t DataBuffer::read_uint32(ByteOrder order)
{
    return read_integer<std::uint32_t>(order);
}
std::int32_t DataBuffer::read_int32(ByteOrder order)
{
    return read_integer<std::int32_t>(order);
}
std::uint64_t DataBuffer::read_uint64(ByteOrder order)
{
    return read_integer<std::uint64_t>(order);
}
std::int64_t DataBuffer::read_int64(ByteOrder order)
{
    return read_integer<std::int64_t>(order);
}
bool DataBuffer::operator==(const DataBuffer& other) const noexcept
{
    return storage_ == other.storage_;
}

void DataBuffer::write_float32(float value, ByteOrder order)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
    write_uint32(std::bit_cast<std::uint32_t>(value), order);
}

float DataBuffer::read_float32(ByteOrder order)
{
    return std::bit_cast<float>(read_uint32(order));
}

} // namespace tcp_helper
