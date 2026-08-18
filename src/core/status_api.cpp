#include "pixelstatus/status_api.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace pixelstatus {
namespace {

using Json = nlohmann::json;
constexpr std::string_view status_path = "/api/v1/status";

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::optional<std::string_view> header_value(
    const ApiRequest& request,
    std::string_view name) {
    const auto wanted = ascii_lower(name);
    for (const auto& [key, value] : request.headers) {
        if (ascii_lower(key) == wanted) {
            return value;
        }
    }
    return std::nullopt;
}

bool constant_time_equal(std::string_view left, std::string_view right) {
    std::size_t difference = left.size() ^ right.size();
    const auto count = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto left_byte = index < left.size()
            ? static_cast<unsigned char>(left[index])
            : 0U;
        const auto right_byte = index < right.size()
            ? static_cast<unsigned char>(right[index])
            : 0U;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0U;
}

bool is_json_content_type(std::string_view value) {
    const auto separator = value.find(';');
    auto media_type = value.substr(0, separator);
    while (!media_type.empty() && std::isspace(static_cast<unsigned char>(media_type.back()))) {
        media_type.remove_suffix(1);
    }
    std::size_t first{};
    while (first < media_type.size()
        && std::isspace(static_cast<unsigned char>(media_type[first]))) {
        ++first;
    }
    return ascii_lower(media_type.substr(first)) == "application/json";
}

ApiResponse json_response(int status, Json body) {
    return ApiResponse{status, "application/json", {}, body.dump()};
}

ApiResponse error_response(int status, std::string message) {
    return json_response(status, Json{{"error", std::move(message)}});
}

bool valid_identifier(std::string_view value, std::size_t maximum_length) {
    if (value.empty() || value.size() > maximum_length) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        const auto is_ascii_alphanumeric =
            (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9');
        return is_ascii_alphanumeric || character == '-' || character == '_'
            || character == '.';
    });
}

Json state_value_json(const StateValue& value) {
    return std::visit(
        [](const auto& current) -> Json {
            using ValueType = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<ValueType, std::monostate>) {
                return nullptr;
            } else {
                return current;
            }
        },
        value);
}

Json resolved_state_json(const ResolvedState& resolved, TimePoint now) {
    auto age = std::chrono::duration_cast<Duration>(now - resolved.state.updated_at);
    if (age < Duration::zero()) {
        age = Duration::zero();
    }
    Json body{
        {"id", resolved.state.id},
        {"status", resolved.effective_status},
        {"reported_status", resolved.state.status},
        {"value", state_value_json(resolved.state.value)},
        {"message", resolved.state.message},
        {"age_ms", age.count()},
        {"stale", resolved.freshness_expired},
    };
    if (resolved.state.ttl) {
        body["ttl_ms"] = resolved.state.ttl->count();
    } else {
        body["ttl_ms"] = nullptr;
    }
    return body;
}

std::optional<StateValue> parse_state_value(
    const Json& value,
    const StatusApiLimits& limits,
    std::string& error) {
    if (value.is_null()) {
        return StateValue{std::monostate{}};
    }
    if (value.is_boolean()) {
        return StateValue{value.get<bool>()};
    }
    if (value.is_number_integer()) {
        if (value.is_number_unsigned()) {
            const auto unsigned_value = value.get<std::uint64_t>();
            if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                error = "value integer is out of range";
                return std::nullopt;
            }
            return StateValue{static_cast<std::int64_t>(unsigned_value)};
        }
        return StateValue{value.get<std::int64_t>()};
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number)) {
            error = "value must be finite";
            return std::nullopt;
        }
        return StateValue{number};
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > limits.maximum_string_value_bytes) {
            error = "value string is too long";
            return std::nullopt;
        }
        return StateValue{text};
    }
    error = "value must be null, boolean, number, or string";
    return std::nullopt;
}

std::optional<std::string> path_identifier(std::string_view target) {
    if (target == status_path) {
        return std::nullopt;
    }
    const auto prefix = std::string(status_path) + '/';
    if (!target.starts_with(prefix) || target.size() == prefix.size()) {
        return std::nullopt;
    }
    return std::string(target.substr(prefix.size()));
}

}  // namespace

StatusApi::StatusApi(StateStore& states, std::string bearer_token, StatusApiLimits limits)
    : states_(states), bearer_token_(std::move(bearer_token)), limits_(limits) {}

ApiResponse StatusApi::handle(const ApiRequest& request, TimePoint now) {
    const auto authorization = header_value(request, "authorization");
    const auto expected = std::string("Bearer ") + bearer_token_;
    if (!authorization || bearer_token_.empty() || !constant_time_equal(*authorization, expected)) {
        auto response = error_response(401, "authentication required");
        response.headers.emplace("WWW-Authenticate", "Bearer");
        return response;
    }

    const auto method = ascii_lower(request.method);
    const auto id_from_path = path_identifier(request.target);
    const auto route_is_collection = request.target == status_path;
    const auto route_is_item = id_from_path.has_value();
    if (!route_is_collection && !route_is_item) {
        return error_response(404, "resource not found");
    }

    if (method == "get") {
        if (route_is_item) {
            if (!valid_identifier(*id_from_path, limits_.maximum_id_bytes)) {
                return error_response(400, "invalid status id");
            }
            const auto state = states_.resolve(*id_from_path, now);
            if (!state) {
                return error_response(404, "status not found");
            }
            return json_response(200, resolved_state_json(*state, now));
        }

        Json statuses = Json::array();
        for (const auto& state : states_.snapshot(now)) {
            statuses.push_back(resolved_state_json(state, now));
        }
        return json_response(200, Json{{"statuses", std::move(statuses)}});
    }

    if (method != "post") {
        auto response = error_response(405, "method not allowed");
        response.headers.emplace("Allow", "GET, POST");
        return response;
    }

    if (request.body.size() > limits_.maximum_body_bytes) {
        return error_response(413, "request body is too large");
    }
    const auto content_type = header_value(request, "content-type");
    if (!content_type || !is_json_content_type(*content_type)) {
        return error_response(415, "content type must be application/json");
    }

    const auto body = Json::parse(request.body, nullptr, false, true);
    if (body.is_discarded() || !body.is_object()) {
        return error_response(400, "request body must be a valid JSON object");
    }
    for (auto field = body.begin(); field != body.end(); ++field) {
        if (field.key() != "id" && field.key() != "status" && field.key() != "value"
            && field.key() != "message" && field.key() != "ttl") {
            return error_response(400, "unknown field: " + field.key());
        }
    }

    std::string id;
    if (route_is_item) {
        id = *id_from_path;
        if (const auto body_id = body.find("id"); body_id != body.end()) {
            if (!body_id->is_string() || body_id->get_ref<const std::string&>() != id) {
                return error_response(400, "body id must match path id");
            }
        }
    } else {
        const auto body_id = body.find("id");
        if (body_id == body.end() || !body_id->is_string()) {
            return error_response(400, "id is required");
        }
        id = body_id->get<std::string>();
    }
    if (!valid_identifier(id, limits_.maximum_id_bytes)) {
        return error_response(400, "invalid status id");
    }

    const auto status_field = body.find("status");
    if (status_field == body.end() || !status_field->is_string()) {
        return error_response(400, "status is required");
    }
    const auto status = status_field->get<std::string>();
    if (!valid_identifier(status, limits_.maximum_status_bytes)) {
        return error_response(400, "invalid status name");
    }

    std::string message;
    if (const auto field = body.find("message"); field != body.end()) {
        if (!field->is_string() || field->get_ref<const std::string&>().size() > limits_.maximum_message_bytes) {
            return error_response(400, "message must be a string within the configured limit");
        }
        message = field->get<std::string>();
    }

    StateValue value;
    if (const auto field = body.find("value"); field != body.end()) {
        std::string value_error;
        auto parsed_value = parse_state_value(*field, limits_, value_error);
        if (!parsed_value) {
            return error_response(400, std::move(value_error));
        }
        value = std::move(*parsed_value);
    }

    std::optional<Duration> ttl;
    if (const auto field = body.find("ttl"); field != body.end()) {
        if (!field->is_number_integer()) {
            return error_response(400, "ttl must be a positive integer number of seconds");
        }
        std::uint64_t seconds{};
        if (field->is_number_unsigned()) {
            seconds = field->get<std::uint64_t>();
        } else {
            const auto signed_seconds = field->get<std::int64_t>();
            if (signed_seconds <= 0) {
                return error_response(400, "ttl must be a positive integer number of seconds");
            }
            seconds = static_cast<std::uint64_t>(signed_seconds);
        }
        const auto maximum_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            limits_.maximum_ttl).count();
        if (seconds == 0U || seconds > static_cast<std::uint64_t>(maximum_seconds)) {
            return error_response(400, "ttl exceeds the configured limit");
        }
        ttl = std::chrono::duration_cast<Duration>(std::chrono::seconds(seconds));
    }

    std::scoped_lock update_lock(update_mutex_);
    const auto existed = states_.find(id).has_value();
    if (!existed && states_.size() >= limits_.maximum_states) {
        return error_response(507, "status store limit reached");
    }
    MonitorState state;
    state.id = id;
    state.status = status;
    state.value = std::move(value);
    state.message = std::move(message);
    state.observed_at = now;
    state.updated_at = now;
    state.ttl = ttl;
    if (!states_.upsert(std::move(state))) {
        return error_response(500, "unable to update state");
    }

    const auto resolved = states_.resolve(id, now);
    return json_response(existed ? 200 : 201, resolved_state_json(*resolved, now));
}

}  // namespace pixelstatus
