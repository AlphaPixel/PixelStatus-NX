#pragma once

#include "pixelstatus/output_driver.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace pixelstatus::simulator {

struct Win32OutputDriverImpl;

class Win32OutputDriver final : public OutputDriver {
public:
    Win32OutputDriver(std::size_t width, std::size_t height, std::wstring title);
    ~Win32OutputDriver() override;

    Win32OutputDriver(const Win32OutputDriver&) = delete;
    Win32OutputDriver& operator=(const Win32OutputDriver&) = delete;

    [[nodiscard]] bool begin() override;
    [[nodiscard]] FrameSubmitResult submit_frame(const Frame& frame) override;
    [[nodiscard]] DriverState state() const override;

    [[nodiscard]] bool process_events();

private:
    std::unique_ptr<Win32OutputDriverImpl> impl_;
};

}  // namespace pixelstatus::simulator
