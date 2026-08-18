#include "pixelstatus/appearance.hpp"

#include <algorithm>
#include <cstddef>

namespace pixelstatus {

TimelineAppearance::TimelineAppearance()
    : keyframes_{{Duration::zero(), Rgb{}, Transition::step}} {}

TimelineAppearance::TimelineAppearance(
    std::vector<ColorKeyframe> keyframes,
    Duration cycle,
    bool repeat)
    : keyframes_(std::move(keyframes)), cycle_(cycle), repeat_(repeat) {
    if (keyframes_.empty()) {
        keyframes_.push_back({Duration::zero(), Rgb{}, Transition::step});
    }
    std::sort(keyframes_.begin(), keyframes_.end(), [](const auto& left, const auto& right) {
        return left.at < right.at;
    });
    if (cycle_ < keyframes_.back().at) {
        cycle_ = keyframes_.back().at;
    }
}

TimelineAppearance TimelineAppearance::solid(Rgb color) {
    return TimelineAppearance({{Duration::zero(), color, Transition::step}}, Duration::zero(), false);
}

TimelineAppearance TimelineAppearance::blink(Rgb color, Duration on, Duration off) {
    return TimelineAppearance(
        {
            {Duration::zero(), color, Transition::step},
            {on, Rgb{}, Transition::step},
        },
        on + off,
        true);
}

Rgb TimelineAppearance::sample(Duration elapsed) const {
    if (elapsed < Duration::zero()) {
        elapsed = Duration::zero();
    }

    auto phase = elapsed;
    if (repeat_ && cycle_ > Duration::zero()) {
        phase = Duration{elapsed.count() % cycle_.count()};
    } else if (cycle_ > Duration::zero() && phase > cycle_) {
        phase = cycle_;
    }

    std::size_t current_index{};
    for (std::size_t index = 1; index < keyframes_.size(); ++index) {
        if (keyframes_[index].at > phase) {
            break;
        }
        current_index = index;
    }

    const auto& current = keyframes_[current_index];
    if (current.transition_to_next == Transition::step) {
        return current.color;
    }

    const auto wraps = current_index + 1 >= keyframes_.size();
    if (wraps && (!repeat_ || cycle_ <= current.at)) {
        return current.color;
    }

    const auto& next = wraps ? keyframes_.front() : keyframes_[current_index + 1];
    const auto next_time = wraps ? cycle_ : next.at;
    const auto interval = next_time - current.at;
    if (interval <= Duration::zero()) {
        return next.color;
    }
    const auto amount = static_cast<double>((phase - current.at).count())
        / static_cast<double>(interval.count());
    return interpolate(current.color, next.color, amount);
}

const std::vector<ColorKeyframe>& TimelineAppearance::keyframes() const noexcept {
    return keyframes_;
}

Duration TimelineAppearance::cycle() const noexcept {
    return cycle_;
}

bool TimelineAppearance::repeats() const noexcept {
    return repeat_;
}

}  // namespace pixelstatus
