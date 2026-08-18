#include "pixelstatus/validation.hpp"

#include <algorithm>

namespace pixelstatus {

bool is_valid_identifier(std::string_view value, std::size_t maximum_length) noexcept {
    return !value.empty() && value.size() <= maximum_length
        && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            const auto is_ascii_alphanumeric =
                (character >= 'A' && character <= 'Z')
                || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9');
            return is_ascii_alphanumeric || character == '-'
                || character == '_' || character == '.';
        });
}

}  // namespace pixelstatus
