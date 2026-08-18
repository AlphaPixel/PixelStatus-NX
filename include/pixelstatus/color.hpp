#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pixelstatus {

struct Rgb {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};

    friend constexpr bool operator==(const Rgb&, const Rgb&) = default;
};

[[nodiscard]] std::optional<Rgb> parse_rgb_hex(std::string_view value);
[[nodiscard]] std::string to_rgb_hex(Rgb color);
[[nodiscard]] Rgb interpolate(Rgb from, Rgb to, double amount);

}  // namespace pixelstatus
