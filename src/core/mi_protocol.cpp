#include "pixelstatus/mi_protocol.hpp"

#include <algorithm>

namespace pixelstatus::mi {

std::optional<std::uint8_t> xy_to_index(std::size_t x, std::size_t y) {
    if (x >= matrix_width || y >= matrix_height) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(y * matrix_width + x);
}

PixelPacket build_pixel_packet(std::uint8_t pixel_index, Rgb color) {
    auto trailing_index = static_cast<std::uint8_t>(pixel_index + 1U);
    if (pixel_index == 0U) {
        trailing_index = 0xFFU;
    }

    return PixelPacket{
        0xBCU,
        0x01U,
        0x01U,
        0x00U,
        pixel_index,
        color.r,
        color.g,
        color.b,
        trailing_index,
        0x55U,
    };
}

std::optional<BlockPacket> build_block_packet(
    std::size_t block_index,
    std::span<const Rgb> pixels) {
    if (block_index >= block_count || pixels.size() != pixels_per_block) {
        return std::nullopt;
    }

    BlockPacket packet{};
    packet[0] = 0xBCU;
    packet[1] = 0x0FU;
    packet[2] = static_cast<std::uint8_t>(block_index + 1U);

    std::size_t offset = 3;
    for (const auto color : pixels) {
        packet[offset++] = color.r;
        packet[offset++] = color.g;
        packet[offset++] = color.b;
    }
    packet.back() = 0x55U;
    return packet;
}

}  // namespace pixelstatus::mi
