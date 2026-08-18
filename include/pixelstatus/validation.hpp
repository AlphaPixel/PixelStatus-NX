#pragma once

#include <cstddef>
#include <string_view>

namespace pixelstatus {

inline constexpr std::size_t maximum_identifier_bytes = 64U;

[[nodiscard]] bool is_valid_identifier(
    std::string_view value,
    std::size_t maximum_length = maximum_identifier_bytes) noexcept;

}  // namespace pixelstatus
