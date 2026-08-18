#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/frame.hpp"
#include "pixelstatus/state.hpp"

#include <cstddef>
#include <string>

namespace pixelstatus {

struct RenderReport {
    bool success{};
    std::size_t rendered_indicators{};
    std::size_t missing_sources{};
    std::size_t missing_statuses{};
    std::string error;
};

class Renderer {
public:
    explicit Renderer(TimePoint animation_origin);

    [[nodiscard]] RenderReport render(
        const StateStore& states,
        const AppConfig& config,
        TimePoint now,
        Frame& output) const;

private:
    TimePoint animation_origin_{};
};

}  // namespace pixelstatus
