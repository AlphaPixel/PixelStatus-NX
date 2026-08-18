#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pixelstatus {

using Duration = std::chrono::milliseconds;
using TimePoint = std::chrono::steady_clock::time_point;
using StateValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct MonitorState {
    std::string id;
    std::string status;
    StateValue value;
    std::string message;
    TimePoint observed_at{};
    TimePoint updated_at{};
    std::optional<Duration> ttl;
};

struct ResolvedState {
    MonitorState state;
    std::string effective_status;
    TimePoint status_entered_at{};
    bool freshness_expired{};
};

class StateStore {
public:
    [[nodiscard]] bool upsert(MonitorState state);
    [[nodiscard]] std::optional<MonitorState> find(const std::string& id) const;
    [[nodiscard]] std::optional<ResolvedState> resolve(
        const std::string& id,
        TimePoint now) const;
    [[nodiscard]] std::vector<ResolvedState> snapshot(TimePoint now) const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        MonitorState state;
        TimePoint status_entered_at{};
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> states_;
};

}  // namespace pixelstatus
