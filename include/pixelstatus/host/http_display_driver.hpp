#pragma once

#include "pixelstatus/output_driver.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pixelstatus::host {

struct HttpDisplayOptions {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8788};
    std::chrono::milliseconds default_refresh_interval{100};
};

struct HttpDisplayDriverImpl;

class HttpDisplayDriver final : public OutputDriver {
public:
    HttpDisplayDriver(
        std::size_t width,
        std::size_t height,
        HttpDisplayOptions options = {});
    ~HttpDisplayDriver() override;

    HttpDisplayDriver(const HttpDisplayDriver&) = delete;
    HttpDisplayDriver& operator=(const HttpDisplayDriver&) = delete;

    [[nodiscard]] bool begin() override;
    [[nodiscard]] FrameSubmitResult submit_frame(const Frame& frame) override;
    [[nodiscard]] DriverState state() const override;

    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::string url() const;

private:
    std::unique_ptr<HttpDisplayDriverImpl> impl_;
};

}  // namespace pixelstatus::host
