#pragma once

#include "pixelstatus/color.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pixelstatus::mi {

inline constexpr std::size_t matrix_width = 16;
inline constexpr std::size_t matrix_height = 16;
inline constexpr std::size_t pixels_per_block = 32;
inline constexpr std::size_t block_count = 8;

using PixelPacket = std::array<std::uint8_t, 10>;
using BlockPacket = std::array<std::uint8_t, 100>;

[[nodiscard]] std::optional<std::uint8_t> xy_to_index(std::size_t x, std::size_t y);
[[nodiscard]] PixelPacket build_pixel_packet(std::uint8_t pixel_index, Rgb color);
[[nodiscard]] std::optional<BlockPacket> build_block_packet(
    std::size_t block_index,
    std::span<const Rgb> pixels);

}  // namespace pixelstatus::mi
