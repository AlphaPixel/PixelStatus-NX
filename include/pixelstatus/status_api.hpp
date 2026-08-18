#pragma once

#include "pixelstatus/state.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pixelstatus {

struct ApiRequest {
    std::string method;
    std::string target;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct ApiResponse {
    int status{500};
    std::string content_type{"application/json"};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct StatusApiLimits {
    std::size_t maximum_body_bytes{4U * 1024U};
    std::size_t maximum_id_bytes{64U};
    std::size_t maximum_status_bytes{64U};
    std::size_t maximum_message_bytes{512U};
    std::size_t maximum_string_value_bytes{1024U};
    std::size_t maximum_states{256U};
    Duration maximum_ttl{std::chrono::hours(24 * 7)};
};

class StatusApi {
public:
    StatusApi(StateStore& states, std::string bearer_token, StatusApiLimits limits = {});

    [[nodiscard]] ApiResponse handle(const ApiRequest& request, TimePoint now);

private:
    StateStore& states_;
    std::string bearer_token_;
    StatusApiLimits limits_;
    std::mutex update_mutex_;
};

}  // namespace pixelstatus
