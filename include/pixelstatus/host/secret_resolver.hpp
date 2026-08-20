#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pixelstatus::host {

using SecretResolver =
    std::function<std::optional<std::string>(std::string_view name)>;

[[nodiscard]] SecretResolver system_secret_resolver();

}  // namespace pixelstatus::host
