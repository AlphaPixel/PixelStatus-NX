#include "pixelstatus/state.hpp"

#include <utility>

namespace pixelstatus {

bool StateStore::upsert(MonitorState state) {
    if (state.id.empty() || state.status.empty()) {
        return false;
    }
    if (state.ttl && *state.ttl < Duration::zero()) {
        return false;
    }

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
    const auto found = states_.find(id);
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second.state;
}

std::optional<ResolvedState> StateStore::resolve(const std::string& id, TimePoint now) const {
    const auto found = states_.find(id);
    if (found == states_.end()) {
        return std::nullopt;
    }

    ResolvedState resolved{
        found->second.state,
        found->second.state.status,
        found->second.status_entered_at,
        false,
    };

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

std::size_t StateStore::size() const noexcept {
    return states_.size();
}

}  // namespace pixelstatus
