#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pixelstatus {

enum class HttpScheme {
    http,
    https,
};

struct ParsedHttpUrl {
    HttpScheme scheme{HttpScheme::http};
    std::string base;
    std::string target;
};

[[nodiscard]] std::optional<ParsedHttpUrl> parse_http_url(std::string_view url);

}  // namespace pixelstatus
