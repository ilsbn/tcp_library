#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

inline std::uint16_t parse_port(std::string_view text, bool allow_zero = false)
{
    unsigned int port = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), port);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || port > 65535 ||
        (!allow_zero && port == 0))
    {
        throw std::invalid_argument("port must be an integer in the valid TCP port range");
    }
    return static_cast<std::uint16_t>(port);
}
