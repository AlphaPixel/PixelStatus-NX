#pragma once

#include "pixelstatus/frame.hpp"

#include <cstdint>
#include <string>

namespace pixelstatus {

enum class FrameSubmitResult {
    accepted,
    coalesced,
    unavailable,
};

enum class DriverConnectionState {
    stopped,
    ready,
    reconnecting,
    failed,
};

struct DriverState {
    DriverConnectionState connection{DriverConnectionState::stopped};
    std::string detail;
    std::uint64_t submitted_frames{};
    std::uint64_t coalesced_frames{};
};

class OutputDriver {
public:
    virtual ~OutputDriver() = default;

    [[nodiscard]] virtual bool begin() = 0;
    [[nodiscard]] virtual FrameSubmitResult submit_frame(const Frame& frame) = 0;
    [[nodiscard]] virtual DriverState state() const = 0;
};

}  // namespace pixelstatus
