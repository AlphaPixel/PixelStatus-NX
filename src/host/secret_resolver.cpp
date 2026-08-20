#include "pixelstatus/host/secret_resolver.hpp"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace pixelstatus::host {
namespace {

std::string environment_name(std::string_view name) {
    std::string result{"PIXELSTATUS_SECRET_"};
    result.reserve(result.size() + name.size());
    for (const auto character : name) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) != 0
            ? static_cast<char>(std::toupper(byte))
            : '_');
    }
    return result;
}

std::optional<std::string> environment_secret(std::string_view name) {
    const auto variable = environment_name(name);
#ifdef _WIN32
    char* value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, variable.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    return std::string(value, length == 0U ? 0U : length - 1U);
#else
    if (const auto* value = std::getenv(variable.c_str())) {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

#ifdef _WIN32
std::optional<std::string> credential_manager_secret(std::string_view name) {
    std::wstring target{L"PixelStatus-NX/"};
    target.append(name.begin(), name.end());
    PCREDENTIALW credential{};
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return std::nullopt;
    }
    const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> owned(
        credential, &CredFree);
    const auto* bytes = credential->CredentialBlob;
    const auto size = static_cast<std::size_t>(credential->CredentialBlobSize);
    if (bytes == nullptr || size == 0U) {
        return std::string{};
    }

    bool looks_utf16 = size % sizeof(wchar_t) == 0U;
    if (looks_utf16) {
        looks_utf16 = false;
        for (std::size_t index = 1U; index < size; index += sizeof(wchar_t)) {
            if (bytes[index] == 0U) {
                looks_utf16 = true;
                break;
            }
        }
    }
    if (!looks_utf16) {
        return std::string(reinterpret_cast<const char*>(bytes), size);
    }

    const auto* wide = reinterpret_cast<const wchar_t*>(bytes);
    const auto wide_size = static_cast<int>(size / sizeof(wchar_t));
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide, wide_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide,
            wide_size,
            value.data(),
            required,
            nullptr,
            nullptr)
        != required) {
        return std::nullopt;
    }
    return value;
}
#endif

}  // namespace

SecretResolver system_secret_resolver() {
    return [](std::string_view name) -> std::optional<std::string> {
        if (auto value = environment_secret(name)) {
            return value;
        }
#ifdef _WIN32
        return credential_manager_secret(name);
#else
        return std::nullopt;
#endif
    };
}

}  // namespace pixelstatus::host
