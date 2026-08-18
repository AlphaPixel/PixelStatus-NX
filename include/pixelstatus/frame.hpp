#pragma once

#include "pixelstatus/color.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace pixelstatus {

class Frame {
public:
    Frame() = default;
    Frame(std::size_t width, std::size_t height, Rgb initial = {});

    [[nodiscard]] std::size_t width() const noexcept;
    [[nodiscard]] std::size_t height() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    void fill(Rgb color);
    [[nodiscard]] bool set_pixel(std::size_t x, std::size_t y, Rgb color);
    [[nodiscard]] const Rgb* pixel(std::size_t x, std::size_t y) const noexcept;
    [[nodiscard]] bool fill_rect(
        std::size_t x,
        std::size_t y,
        std::size_t width,
        std::size_t height,
        Rgb color);

    [[nodiscard]] std::span<const Rgb> pixels() const noexcept;

private:
    std::size_t width_{};
    std::size_t height_{};
    std::vector<Rgb> pixels_;
};

}  // namespace pixelstatus
