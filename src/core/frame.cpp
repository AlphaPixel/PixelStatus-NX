#include "pixelstatus/frame.hpp"

#include <algorithm>

namespace pixelstatus {

Frame::Frame(std::size_t width, std::size_t height, Rgb initial)
    : width_(width), height_(height), pixels_(width * height, initial) {}

std::size_t Frame::width() const noexcept {
    return width_;
}

std::size_t Frame::height() const noexcept {
    return height_;
}

std::size_t Frame::size() const noexcept {
    return pixels_.size();
}

void Frame::fill(Rgb color) {
    std::fill(pixels_.begin(), pixels_.end(), color);
}

bool Frame::set_pixel(std::size_t x, std::size_t y, Rgb color) {
    if (x >= width_ || y >= height_) {
        return false;
    }
    pixels_[y * width_ + x] = color;
    return true;
}

const Rgb* Frame::pixel(std::size_t x, std::size_t y) const noexcept {
    if (x >= width_ || y >= height_) {
        return nullptr;
    }
    return &pixels_[y * width_ + x];
}

bool Frame::fill_rect(
    std::size_t x,
    std::size_t y,
    std::size_t width,
    std::size_t height,
    Rgb color) {
    if (width == 0 || height == 0 || x >= width_ || y >= height_) {
        return false;
    }
    if (width > width_ - x || height > height_ - y) {
        return false;
    }

    for (std::size_t row = y; row < y + height; ++row) {
        const auto begin = pixels_.begin() + static_cast<std::ptrdiff_t>(row * width_ + x);
        std::fill(begin, begin + static_cast<std::ptrdiff_t>(width), color);
    }
    return true;
}

std::span<const Rgb> Frame::pixels() const noexcept {
    return pixels_;
}

}  // namespace pixelstatus
