#include "pixelstatus/renderer.hpp"

#include <chrono>

namespace pixelstatus {

Renderer::Renderer(TimePoint animation_origin) : animation_origin_(animation_origin) {}

RenderReport Renderer::render(
    const StateStore& states,
    const AppConfig& config,
    TimePoint now,
    Frame& output) const {
    RenderReport report;
    if (output.width() != config.display.width || output.height() != config.display.height) {
        report.error = "Output frame dimensions do not match display configuration";
        return report;
    }

    output.fill(config.display.background);
    for (const auto& indicator : config.indicators) {
        const auto resolved = states.resolve(indicator.source, now);
        std::string status = "unknown";
        auto entered_at = animation_origin_;
        if (resolved) {
            status = resolved->effective_status;
            entered_at = resolved->status_entered_at;
        } else {
            ++report.missing_sources;
        }

        auto appearance = config.statuses.find(status);
        if (appearance == config.statuses.end()) {
            ++report.missing_statuses;
            appearance = config.statuses.find("unknown");
        }
        if (appearance == config.statuses.end()) {
            continue;
        }

        auto elapsed = std::chrono::duration_cast<Duration>(now - entered_at);
        if (elapsed < Duration::zero()) {
            elapsed = Duration::zero();
        }
        const auto color = appearance->second.sample(elapsed);
        if (!output.fill_rect(
                indicator.x,
                indicator.y,
                indicator.width,
                indicator.height,
                color)) {
            report.error = "Indicator layout became invalid during rendering";
            return report;
        }
        ++report.rendered_indicators;
    }

    report.success = true;
    return report;
}

}  // namespace pixelstatus
