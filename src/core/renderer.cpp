#include "pixelstatus/renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <optional>
#include <variant>

namespace pixelstatus {
namespace {

constexpr std::array<std::array<std::uint8_t, 7U>, 10U> digit_glyphs{{
    {{0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111}},
    {{0b010, 0b110, 0b010, 0b010, 0b010, 0b010, 0b111}},
    {{0b111, 0b001, 0b001, 0b111, 0b100, 0b100, 0b111}},
    {{0b111, 0b001, 0b001, 0b111, 0b001, 0b001, 0b111}},
    {{0b101, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001}},
    {{0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b111}},
    {{0b111, 0b100, 0b100, 0b111, 0b101, 0b101, 0b111}},
    {{0b111, 0b001, 0b001, 0b010, 0b010, 0b010, 0b010}},
    {{0b111, 0b101, 0b101, 0b111, 0b101, 0b101, 0b111}},
    {{0b111, 0b101, 0b101, 0b111, 0b001, 0b001, 0b111}},
}};

void merge_report(RenderReport& target, const RenderReport& source) {
    target.rendered_indicators += source.rendered_indicators;
    target.missing_sources += source.missing_sources;
    target.missing_statuses += source.missing_statuses;
    target.invalid_values += source.invalid_values;
    if (!source.success && target.error.empty()) {
        target.error = source.error;
    }
}

struct SourceSample {
    std::optional<ResolvedState> state;
    std::optional<Rgb> color;
};

SourceSample sample_source(
    const StateStore& states,
    const AppConfig& config,
    const std::string& source,
    TimePoint now,
    TimePoint animation_origin,
    RenderReport& report) {
    SourceSample sample;
    sample.state = states.resolve(source, now);
    std::string status = "unknown";
    auto entered_at = animation_origin;
    if (sample.state) {
        status = sample.state->effective_status;
        entered_at = sample.state->status_entered_at;
    } else {
        ++report.missing_sources;
    }

    auto appearance = config.statuses.find(status);
    if (appearance == config.statuses.end()) {
        ++report.missing_statuses;
        appearance = config.statuses.find("unknown");
    }
    if (appearance == config.statuses.end()) {
        return sample;
    }

    auto elapsed = std::chrono::duration_cast<Duration>(now - entered_at);
    if (elapsed < Duration::zero()) {
        elapsed = Duration::zero();
    }
    sample.color = appearance->second.sample(elapsed);
    return sample;
}

bool render_indicator(
    const StateStore& states,
    const AppConfig& config,
    const IndicatorConfig& indicator,
    TimePoint now,
    TimePoint animation_origin,
    RenderReport& report,
    Frame& output) {
    const auto sample = sample_source(
        states, config, indicator.source, now, animation_origin, report);
    if (!sample.color) {
        return true;
    }
    if (!output.fill_rect(
            indicator.x,
            indicator.y,
            indicator.width,
            indicator.height,
            *sample.color)) {
        report.error = "Indicator layout became invalid during rendering";
        return false;
    }
    ++report.rendered_indicators;
    return true;
}

[[nodiscard]] std::optional<double> numeric_value(const StateValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    if (const auto* real = std::get_if<double>(&value); real != nullptr
        && std::isfinite(*real)) {
        return *real;
    }
    return std::nullopt;
}

bool render_bar(
    const StateStore& states,
    const AppConfig& config,
    const LayoutBarConfig& bar,
    TimePoint now,
    TimePoint animation_origin,
    RenderReport& report,
    Frame& output) {
    if (!output.fill_rect(
            bar.x, bar.y, bar.width, bar.height, bar.track_color)) {
        report.error = "Layout bar bounds became invalid during rendering";
        return false;
    }
    const auto sample = sample_source(
        states, config, bar.source, now, animation_origin, report);
    ++report.rendered_indicators;
    if (!sample.state || !sample.color) {
        return true;
    }
    const auto value = numeric_value(sample.state->state.value);
    if (!value) {
        ++report.invalid_values;
        return true;
    }
    const auto raw_fraction =
        (*value - bar.minimum) / (bar.maximum - bar.minimum);
    if (!std::isfinite(raw_fraction)) {
        ++report.invalid_values;
        return true;
    }
    const auto fraction = std::clamp(raw_fraction, 0.0, 1.0);
    const auto horizontal = bar.direction == BarDirection::right
        || bar.direction == BarDirection::left;
    const auto extent = horizontal ? bar.width : bar.height;
    const auto filled = std::min(
        extent,
        static_cast<std::size_t>(std::floor(fraction * static_cast<double>(extent) + 0.5)));
    if (filled == 0U) {
        return true;
    }
    auto x = bar.x;
    auto y = bar.y;
    auto width = bar.width;
    auto height = bar.height;
    if (horizontal) {
        width = filled;
        if (bar.direction == BarDirection::left) {
            x += bar.width - filled;
        }
    } else {
        height = filled;
        if (bar.direction == BarDirection::up) {
            y += bar.height - filled;
        }
    }
    if (!output.fill_rect(x, y, width, height, *sample.color)) {
        report.error = "Layout bar fill became invalid during rendering";
        return false;
    }
    return true;
}

bool render_status_grid(
    const StateStore& states,
    const AppConfig& config,
    const LayoutStatusGridConfig& grid,
    TimePoint now,
    TimePoint animation_origin,
    RenderReport& report,
    Frame& output) {
    if (grid.sources.empty() || grid.columns == 0U
        || grid.columns > grid.sources.size()) {
        report.error = "Layout status grid configuration became invalid during rendering";
        return false;
    }
    const auto rows = (grid.sources.size() + grid.columns - 1U) / grid.columns;
    const auto horizontal_gaps = grid.gap * (grid.columns - 1U);
    const auto vertical_gaps = grid.gap * (rows - 1U);
    if (horizontal_gaps > grid.width || grid.width - horizontal_gaps < grid.columns
        || vertical_gaps > grid.height || grid.height - vertical_gaps < rows) {
        report.error = "Layout status grid geometry became invalid during rendering";
        return false;
    }
    const auto usable_width = grid.width - horizontal_gaps;
    const auto usable_height = grid.height - vertical_gaps;
    for (std::size_t index = 0; index < grid.sources.size(); ++index) {
        const auto column = index % grid.columns;
        const auto row = index / grid.columns;
        const auto left = usable_width * column / grid.columns;
        const auto right = usable_width * (column + 1U) / grid.columns;
        const auto top = usable_height * row / rows;
        const auto bottom = usable_height * (row + 1U) / rows;
        const auto sample = sample_source(
            states, config, grid.sources[index], now, animation_origin, report);
        ++report.rendered_indicators;
        if (!sample.color) {
            continue;
        }
        if (!output.fill_rect(
                grid.x + left + column * grid.gap,
                grid.y + top + row * grid.gap,
                right - left,
                bottom - top,
                *sample.color)) {
            report.error = "Layout status grid bounds became invalid during rendering";
            return false;
        }
    }
    return true;
}

bool render_aggregate_status(
    const StateStore& states,
    const LayoutAggregateStatusConfig& aggregate,
    TimePoint now,
    RenderReport& report,
    Frame& output) {
    auto selected = aggregate.priority.size();
    for (const auto& source : aggregate.sources) {
        const auto resolved = states.resolve(source, now);
        const auto status = resolved ? resolved->effective_status : std::string("unknown");
        if (!resolved) {
            ++report.missing_sources;
        }
        const auto found = std::find(
            aggregate.priority.begin(), aggregate.priority.end(), status);
        if (found == aggregate.priority.end()) {
            continue;
        }
        selected = std::min(
            selected,
            static_cast<std::size_t>(found - aggregate.priority.begin()));
    }

    auto color = aggregate.default_color;
    if (selected < aggregate.priority.size()) {
        const auto found = aggregate.colors.find(aggregate.priority[selected]);
        if (found == aggregate.colors.end()) {
            report.error = "Aggregate status color became invalid during rendering";
            return false;
        }
        color = found->second;
    }
    if (!output.fill_rect(
            aggregate.x,
            aggregate.y,
            aggregate.width,
            aggregate.height,
            color)) {
        report.error = "Aggregate status bounds became invalid during rendering";
        return false;
    }
    ++report.rendered_indicators;
    return true;
}

bool render_layout_bitmap(
    const LayoutBitmapConfig& bitmap,
    RenderReport& report,
    Frame& output) {
    for (std::size_t y = 0; y < bitmap.pixels.size(); ++y) {
        for (std::size_t x = 0; x < bitmap.pixels[y].size(); ++x) {
            const auto color = bitmap.palette.find(bitmap.pixels[y][x]);
            if (color == bitmap.palette.end()
                || !output.set_pixel(bitmap.x + x, bitmap.y + y, color->second)) {
                report.error = "Layout bitmap became invalid during rendering";
                return false;
            }
        }
    }
    return true;
}

RenderReport render_indicators(
    const StateStore& states,
    const AppConfig& config,
    const std::vector<IndicatorConfig>& indicators,
    TimePoint now,
    TimePoint animation_origin,
    Frame& output) {
    RenderReport report;
    output.fill(config.display.background);
    for (const auto& indicator : indicators) {
        if (!render_indicator(
                states, config, indicator, now, animation_origin, report, output)) {
            return report;
        }
    }
    report.success = true;
    return report;
}

void draw_digit(
    Frame& output,
    std::size_t x,
    std::size_t y,
    unsigned int digit,
    Rgb color) {
    const auto& glyph = digit_glyphs[digit];
    for (std::size_t row = 0; row < glyph.size(); ++row) {
        for (std::size_t column = 0; column < 3U; ++column) {
            if ((glyph[row] & (1U << (2U - column))) != 0U) {
                static_cast<void>(output.set_pixel(x + column, y + row, color));
            }
        }
    }
}

bool draw_time(
    Frame& output,
    std::size_t x,
    std::size_t y,
    std::size_t width,
    std::size_t height,
    int hour,
    int minute,
    bool show_colon,
    Rgb color) {
    if (width < 15U || height < 7U || x >= output.width()
        || width > output.width() - x || y >= output.height()
        || height > output.height() - y) {
        return false;
    }
    const auto origin_x = x + (width - 15U) / 2U;
    const auto origin_y = y + (height - 7U) / 2U;
    draw_digit(
        output, origin_x, origin_y, static_cast<unsigned int>(hour / 10), color);
    draw_digit(
        output, origin_x + 3U, origin_y, static_cast<unsigned int>(hour % 10), color);
    if (show_colon) {
        static_cast<void>(output.set_pixel(origin_x + 7U, origin_y + 2U, color));
        static_cast<void>(output.set_pixel(origin_x + 7U, origin_y + 4U, color));
    }
    draw_digit(
        output, origin_x + 9U, origin_y, static_cast<unsigned int>(minute / 10), color);
    draw_digit(
        output, origin_x + 12U, origin_y, static_cast<unsigned int>(minute % 10), color);
    return true;
}

bool split_wall_time(
    std::chrono::system_clock::time_point wall_now,
    std::tm& local,
    std::tm& utc) {
    const auto raw = std::chrono::system_clock::to_time_t(wall_now);
#ifdef _WIN32
    return localtime_s(&local, &raw) == 0 && gmtime_s(&utc, &raw) == 0;
#else
    return localtime_r(&raw, &local) != nullptr
        && gmtime_r(&raw, &utc) != nullptr;
#endif
}

RenderReport render_card(
    const StateStore& states,
    const AppConfig& config,
    const CardConfig& card,
    TimePoint now,
    TimePoint animation_origin,
    std::chrono::system_clock::time_point wall_now,
    Frame& output) {
    if (const auto* indicators = std::get_if<IndicatorCardConfig>(&card.content)) {
        return render_indicators(
            states,
            config,
            indicators->indicators,
            now,
            animation_origin,
            output);
    }

    RenderReport report;
    output.fill(config.display.background);
    if (const auto* layout = std::get_if<LayoutCardConfig>(&card.content)) {
        std::tm local{};
        std::tm utc{};
        bool wall_time_loaded{};
        for (const auto& widget : layout->widgets) {
            if (const auto* indicator = std::get_if<IndicatorConfig>(&widget)) {
                if (!render_indicator(
                        states,
                        config,
                        *indicator,
                        now,
                        animation_origin,
                        report,
                        output)) {
                    return report;
                }
                continue;
            }
            if (const auto* bar = std::get_if<LayoutBarConfig>(&widget)) {
                if (!render_bar(
                        states,
                        config,
                        *bar,
                        now,
                        animation_origin,
                        report,
                        output)) {
                    return report;
                }
                continue;
            }
            if (const auto* grid = std::get_if<LayoutStatusGridConfig>(&widget)) {
                if (!render_status_grid(
                        states,
                        config,
                        *grid,
                        now,
                        animation_origin,
                        report,
                        output)) {
                    return report;
                }
                continue;
            }
            if (const auto* aggregate =
                    std::get_if<LayoutAggregateStatusConfig>(&widget)) {
                if (!render_aggregate_status(
                        states, *aggregate, now, report, output)) {
                    return report;
                }
                continue;
            }
            if (const auto* bitmap = std::get_if<LayoutBitmapConfig>(&widget)) {
                if (!render_layout_bitmap(*bitmap, report, output)) {
                    return report;
                }
                continue;
            }

            const auto* clock = std::get_if<LayoutClockConfig>(&widget);
            if (clock == nullptr) {
                report.error = "Layout widget became invalid during rendering";
                return report;
            }
            if (!wall_time_loaded) {
                if (!split_wall_time(wall_now, local, utc)) {
                    report.error = "Layout clock could not convert the current wall time";
                    return report;
                }
                wall_time_loaded = true;
            }
            const auto& selected = clock->timezone == ClockTimeZone::local ? local : utc;
            if (!draw_time(
                    output,
                    clock->x,
                    clock->y,
                    clock->width,
                    clock->height,
                    selected.tm_hour,
                    selected.tm_min,
                    selected.tm_sec % 2 == 0,
                    clock->color)) {
                report.error = "Layout clock bounds became invalid during rendering";
                return report;
            }
        }
        report.success = true;
        return report;
    }
    if (const auto* bitmap = std::get_if<BitmapCardConfig>(&card.content)) {
        for (std::size_t y = 0; y < bitmap->pixels.size(); ++y) {
            for (std::size_t x = 0; x < bitmap->pixels[y].size(); ++x) {
                const auto color = bitmap->palette.find(bitmap->pixels[y][x]);
                if (color == bitmap->palette.end()
                    || !output.set_pixel(x, y, color->second)) {
                    report.error = "Bitmap card became invalid during rendering";
                    return report;
                }
            }
        }
        report.success = true;
        return report;
    }

    const auto* clock = std::get_if<ClockCardConfig>(&card.content);
    std::tm local{};
    std::tm utc{};
    if (clock == nullptr || !split_wall_time(wall_now, local, utc)) {
        report.error = "Clock card could not convert the current wall time";
        return report;
    }
    const auto show_colon = local.tm_sec % 2 == 0;
    static_cast<void>(draw_time(
        output,
        0U,
        0U,
        output.width(),
        7U,
        local.tm_hour,
        local.tm_min,
        show_colon,
        clock->local_color));
    static_cast<void>(draw_time(
        output,
        0U,
        output.height() - 7U,
        output.width(),
        7U,
        utc.tm_hour,
        utc.tm_min,
        show_colon,
        clock->utc_color));
    report.success = true;
    return report;
}

void compose_transition(
    const Frame& from,
    const Frame& to,
    CardTransition transition,
    double progress,
    Frame& output) {
    const auto width = output.width();
    const auto height = output.height();
    if (transition == CardTransition::fade) {
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                static_cast<void>(output.set_pixel(
                    x,
                    y,
                    interpolate(*from.pixel(x, y), *to.pixel(x, y), progress)));
            }
        }
        return;
    }

    const auto horizontal = transition == CardTransition::slide_left
        || transition == CardTransition::slide_right;
    const auto extent = horizontal ? width : height;
    const auto offset = (std::min)(
        extent,
        static_cast<std::size_t>(progress * static_cast<double>(extent)));
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const Rgb* color{};
            if (transition == CardTransition::slide_left) {
                const auto source = x + offset;
                color = source < width
                    ? from.pixel(source, y)
                    : to.pixel(source - width, y);
            } else if (transition == CardTransition::slide_right) {
                color = x >= offset
                    ? from.pixel(x - offset, y)
                    : to.pixel(width - offset + x, y);
            } else if (transition == CardTransition::slide_up) {
                const auto source = y + offset;
                color = source < height
                    ? from.pixel(x, source)
                    : to.pixel(x, source - height);
            } else {
                color = y >= offset
                    ? from.pixel(x, y - offset)
                    : to.pixel(x, height - offset + y);
            }
            static_cast<void>(output.set_pixel(x, y, *color));
        }
    }
}

}  // namespace

Renderer::Renderer(TimePoint animation_origin) : animation_origin_(animation_origin) {}

RenderReport Renderer::render(
    const StateStore& states,
    const AppConfig& config,
    TimePoint now,
    Frame& output) const {
    return render(
        states,
        config,
        now,
        std::chrono::system_clock::now(),
        output);
}

RenderReport Renderer::render(
    const StateStore& states,
    const AppConfig& config,
    TimePoint now,
    std::chrono::system_clock::time_point wall_now,
    Frame& output) const {
    RenderReport report;
    if (output.width() != config.display.width
        || output.height() != config.display.height) {
        report.error = "Output frame dimensions do not match display configuration";
        return report;
    }
    if (config.cards.empty()) {
        return render_indicators(
            states,
            config,
            config.indicators,
            now,
            animation_origin_,
            output);
    }

    if (config.cards.size() == 1U) {
        report = render_card(
            states,
            config,
            config.cards.front(),
            now,
            animation_origin_,
            wall_now,
            output);
        report.active_card = config.cards.front().id;
        return report;
    }

    Duration cycle{};
    for (const auto& card : config.cards) {
        cycle += card.hold + card.transition.duration;
    }
    auto elapsed = std::chrono::duration_cast<Duration>(now - animation_origin_);
    if (elapsed < Duration::zero()) {
        elapsed = Duration::zero();
    }
    auto position = Duration{elapsed.count() % cycle.count()};
    std::size_t active_index{};
    for (; active_index + 1U < config.cards.size(); ++active_index) {
        const auto segment = config.cards[active_index].hold
            + config.cards[active_index].transition.duration;
        if (position < segment) {
            break;
        }
        position -= segment;
    }

    const auto& active = config.cards[active_index];
    report.active_card = active.id;
    if (position < active.hold
        || active.transition.duration == Duration::zero()
        || active.transition.type == CardTransition::instant) {
        auto rendered = render_card(
            states,
            config,
            active,
            now,
            animation_origin_,
            wall_now,
            output);
        rendered.active_card = active.id;
        return rendered;
    }

    const auto next_index = (active_index + 1U) % config.cards.size();
    const auto& next = config.cards[next_index];
    Frame from(config.display.width, config.display.height);
    Frame to(config.display.width, config.display.height);
    const auto from_report = render_card(
        states,
        config,
        active,
        now,
        animation_origin_,
        wall_now,
        from);
    const auto to_report = render_card(
        states,
        config,
        next,
        now,
        animation_origin_,
        wall_now,
        to);
    merge_report(report, from_report);
    merge_report(report, to_report);
    if (!report.error.empty()) {
        return report;
    }
    const auto transition_elapsed = position - active.hold;
    const auto progress = static_cast<double>(transition_elapsed.count())
        / static_cast<double>(active.transition.duration.count());
    compose_transition(from, to, active.transition.type, progress, output);
    report.success = true;
    report.active_card = active.id;
    report.next_card = next.id;
    report.transitioning = true;
    return report;
}

}  // namespace pixelstatus
