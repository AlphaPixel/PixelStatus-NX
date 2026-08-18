#include "pixelstatus/state.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace pixelstatus {
namespace {

ResolvedState resolve_entry(const MonitorState& state, TimePoint entered_at, TimePoint now) {
    ResolvedState resolved{state, state.status, entered_at, false};
    if (resolved.state.ttl) {
        const auto expires_at = resolved.state.updated_at + *resolved.state.ttl;
        if (now >= expires_at) {
            resolved.effective_status = "stale";
            resolved.status_entered_at = expires_at;
            resolved.freshness_expired = true;
        }
    }
    return resolved;
}

}  // namespace

bool StateStore::upsert(MonitorState state) {
    if (state.id.empty() || state.status.empty()) {
        return false;
    }
    if (state.ttl && *state.ttl < Duration::zero()) {
        return false;
    }

    std::unique_lock lock(mutex_);
    auto entered_at = state.updated_at;
    if (const auto existing = states_.find(state.id);
        existing != states_.end() && existing->second.state.status == state.status) {
        const auto was_stale = existing->second.state.ttl
            && state.updated_at >= existing->second.state.updated_at + *existing->second.state.ttl;
        if (!was_stale) {
            entered_at = existing->second.status_entered_at;
        }
    }

    const auto key = state.id;
    states_.insert_or_assign(key, Entry{std::move(state), entered_at});
    return true;
}

std::optional<MonitorState> StateStore::find(const std::string& id) const {
    std::shared_lock lock(mutex_);
    const auto found = states_.find(id);
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second.state;
}

std::optional<ResolvedState> StateStore::resolve(const std::string& id, TimePoint now) const {
    std::shared_lock lock(mutex_);
    const auto found = states_.find(id);
    if (found == states_.end()) {
        return std::nullopt;
    }

    return resolve_entry(found->second.state, found->second.status_entered_at, now);
}

std::vector<ResolvedState> StateStore::snapshot(TimePoint now) const {
    std::shared_lock lock(mutex_);
    std::vector<ResolvedState> result;
    result.reserve(states_.size());
    for (const auto& [id, entry] : states_) {
        static_cast<void>(id);
        result.push_back(resolve_entry(entry.state, entry.status_entered_at, now));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.state.id < right.state.id;
    });
    return result;
}

std::size_t StateStore::size() const {
    std::shared_lock lock(mutex_);
    return states_.size();
}

}  // namespace pixelstatus
