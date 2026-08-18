#pragma once

#include "pixelstatus/color.hpp"
#include "pixelstatus/state.hpp"

#include <vector>

namespace pixelstatus {

enum class Transition {
    step,
    linear,
};

struct ColorKeyframe {
    Duration at{};
    Rgb color{};
    Transition transition_to_next{Transition::step};
};

class TimelineAppearance {
public:
    TimelineAppearance();
    TimelineAppearance(std::vector<ColorKeyframe> keyframes, Duration cycle, bool repeat);

    [[nodiscard]] static TimelineAppearance solid(Rgb color);
    [[nodiscard]] static TimelineAppearance blink(Rgb color, Duration on, Duration off);

    [[nodiscard]] Rgb sample(Duration elapsed) const;
    [[nodiscard]] const std::vector<ColorKeyframe>& keyframes() const noexcept;
    [[nodiscard]] Duration cycle() const noexcept;
    [[nodiscard]] bool repeats() const noexcept;

private:
    std::vector<ColorKeyframe> keyframes_;
    Duration cycle_{};
    bool repeat_{};
};

}  // namespace pixelstatus
