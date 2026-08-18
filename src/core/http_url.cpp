#include "pixelstatus/http_url.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>

namespace pixelstatus {
namespace {

bool valid_port(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    std::uint32_t port{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), port, 10);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
        && port > 0U && port <= 65'535U;
}

bool valid_authority(std::string_view authority) {
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        return bracket != std::string_view::npos && bracket > 1U
            && (bracket + 1U == authority.size()
                || (authority[bracket + 1U] == ':'
                    && valid_port(authority.substr(bracket + 2U))));
    }

    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        return true;
    }
    return colon > 0U && authority.find(':') == colon
        && valid_port(authority.substr(colon + 1U));
}

}  // namespace

std::optional<ParsedHttpUrl> parse_http_url(std::string_view url) {
    constexpr std::string_view http_prefix = "http://";
    constexpr std::string_view https_prefix = "https://";

    ParsedHttpUrl parsed;
    std::size_t authority_start{};
    if (url.starts_with(http_prefix)) {
        parsed.scheme = HttpScheme::http;
        authority_start = http_prefix.size();
    } else if (url.starts_with(https_prefix)) {
        parsed.scheme = HttpScheme::https;
        authority_start = https_prefix.size();
    } else {
        return std::nullopt;
    }
    if (url.find('#') != std::string_view::npos
        || std::any_of(url.begin(), url.end(), [](unsigned char character) {
            return character <= 0x20U || character == 0x7FU;
        })) {
        return std::nullopt;
    }

    const auto target_start = url.find_first_of("/?", authority_start);
    const auto authority = url.substr(
        authority_start,
        target_start == std::string_view::npos
            ? url.size() - authority_start
            : target_start - authority_start);
    if (!valid_authority(authority)) {
        return std::nullopt;
    }

    parsed.base = std::string(url.substr(
        0,
        target_start == std::string_view::npos ? url.size() : target_start));
    if (target_start == std::string_view::npos) {
        parsed.target = "/";
    } else if (url[target_start] == '?') {
        parsed.target = "/" + std::string(url.substr(target_start));
    } else {
        parsed.target = std::string(url.substr(target_start));
    }
    return parsed;
}

}  // namespace pixelstatus
