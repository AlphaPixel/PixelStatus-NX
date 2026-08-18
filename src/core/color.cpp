#include "pixelstatus/color.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace pixelstatus {
namespace {

[[nodiscard]] std::optional<std::uint8_t> parse_byte(std::string_view text) {
    unsigned int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 255U) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

}  // namespace

std::optional<Rgb> parse_rgb_hex(std::string_view value) {
    if (value.size() != 7 || value.front() != '#') {
        return std::nullopt;
    }

    const auto r = parse_byte(value.substr(1, 2));
    const auto g = parse_byte(value.substr(3, 2));
    const auto b = parse_byte(value.substr(5, 2));
    if (!r || !g || !b) {
        return std::nullopt;
    }
    return Rgb{*r, *g, *b};
}

std::string to_rgb_hex(Rgb color) {
    std::ostringstream stream;
    stream << '#' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<unsigned int>(color.r)
           << std::setw(2) << static_cast<unsigned int>(color.g)
           << std::setw(2) << static_cast<unsigned int>(color.b);
    return stream.str();
}

Rgb interpolate(Rgb from, Rgb to, double amount) {
    const auto clamped = std::clamp(amount, 0.0, 1.0);
    const auto channel = [clamped](std::uint8_t start, std::uint8_t end) {
        const auto value = static_cast<double>(start)
            + (static_cast<double>(end) - static_cast<double>(start)) * clamped;
        return static_cast<std::uint8_t>(std::lround(value));
    };
    return Rgb{channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b)};
}

}  // namespace pixelstatus
