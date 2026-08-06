#include "tradebox/platform/credentials.h"

#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>
#include <utility>

namespace {

std::wstring Target(bool paper) {
    return paper ? L"TradeBoxNative/Alpaca/Paper" : L"TradeBoxNative/Alpaca/Live";
}

std::wstring Wide(const std::string& value);

std::wstring Target(std::string_view slot, bool paper) {
    if (slot.empty() || slot == (paper ? "alpaca-paper-default" : "alpaca-live-default"))
        return Target(paper);
    std::string safe_slot;
    for (const char character : slot) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_')
            safe_slot += character;
    }
    std::wstring target = L"TradeBoxNative/Alpaca/Slot/";
    target += paper ? L"Paper/" : L"Live/";
    target += Wide(safe_slot);
    return target;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::string Narrow(const wchar_t* value) {
    if (!value) return {};
    const int length = static_cast<int>(wcslen(value));
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0,
                                         nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), size, nullptr,
                        nullptr);
    return result;
}

std::string Narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
        size, nullptr, nullptr);
    return result;
}

bool ValidCustomSlot(std::string_view slot) {
    if (slot.empty()) return false;
    for (const char character : slot) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_')
            continue;
        return false;
    }
    return true;
}

std::optional<CredentialStore::Descriptor> ParseTarget(
    std::wstring_view target, const wchar_t* environment,
    std::wstring_view prefix) {
    const std::wstring default_target =
        std::wstring(L"TradeBoxNative/Alpaca/") + environment;
    if (target == default_target)
        return CredentialStore::Descriptor{
            .slot = environment == std::wstring_view(L"Paper")
                        ? "alpaca-paper-default"
                        : "alpaca-live-default",
            .paper = environment == std::wstring_view(L"Paper"),
        };

    if (!target.starts_with(prefix) || target.size() == prefix.size())
        return std::nullopt;
    return CredentialStore::Descriptor{
        .slot = Narrow(std::wstring(target.substr(prefix.size()))),
        .paper = environment == std::wstring_view(L"Paper"),
    };
}

}  // namespace

bool CredentialStore::Save(std::string_view slot,
                           const AlpacaCredentials& credentials,
                           std::string& error) {
    const bool is_default =
        slot == (credentials.paper ? "alpaca-paper-default"
                                   : "alpaca-live-default");
    if (!is_default && !ValidCustomSlot(slot)) {
        error = "Account name must use only letters, numbers, '-' or '_'";
        return false;
    }
    const std::wstring target = Target(slot, credentials.paper);
    const std::wstring username = Wide(credentials.key);
    CREDENTIALW value{};
    value.Type = CRED_TYPE_GENERIC;
    value.TargetName = const_cast<wchar_t*>(target.c_str());
    value.UserName = const_cast<wchar_t*>(username.c_str());
    value.CredentialBlobSize = static_cast<DWORD>(credentials.secret.size());
    value.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<char*>(credentials.secret.data()));
    value.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&value, 0)) {
        error = "Windows Credential Manager error " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool CredentialStore::Load(std::string_view slot, bool paper,
                           AlpacaCredentials& credentials, std::string& error) {
    PCREDENTIALW value = nullptr;
    const std::wstring target = Target(slot, paper);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &value)) {
        error = "No saved " + std::string(paper ? "paper" : "live") +
                " Alpaca credentials";
        return false;
    }
    AlpacaCredentials loaded;
    loaded.paper = paper;
    loaded.key = Narrow(value->UserName);
    loaded.secret.assign(
        reinterpret_cast<const char*>(value->CredentialBlob),
        reinterpret_cast<const char*>(value->CredentialBlob) +
            value->CredentialBlobSize);
    if (value->CredentialBlob && value->CredentialBlobSize)
        SecureZeroMemory(value->CredentialBlob, value->CredentialBlobSize);
    CredFree(value);
    credentials = std::move(loaded);
    return true;
}

bool CredentialStore::Exists(std::string_view slot, bool paper) {
    PCREDENTIALW value = nullptr;
    const std::wstring target = Target(slot, paper);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &value))
        return false;
    CredFree(value);
    return true;
}

std::expected<std::vector<CredentialStore::Descriptor>, std::string>
CredentialStore::List() {
    PCREDENTIALW* values = nullptr;
    DWORD count = 0;
    if (!CredEnumerateW(L"TradeBoxNative/Alpaca/*", 0, &count, &values)) {
        const DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) return std::vector<Descriptor>{};
        return std::unexpected(
            "Windows Credential Manager error " + std::to_string(code));
    }

    std::vector<Descriptor> result;
    result.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        const CREDENTIALW& value = *values[index];
        const std::wstring_view target(value.TargetName ? value.TargetName : L"");
        std::optional<Descriptor> descriptor = ParseTarget(
            target, L"Paper", L"TradeBoxNative/Alpaca/Slot/Paper/");
        if (!descriptor)
            descriptor = ParseTarget(
                target, L"Live", L"TradeBoxNative/Alpaca/Slot/Live/");
        if (!descriptor || descriptor->slot.empty()) continue;
        descriptor->api_key_id = Narrow(value.UserName);
        result.push_back(std::move(*descriptor));
    }
    CredFree(values);

    std::ranges::sort(result, {}, [](const Descriptor& descriptor) {
        return std::pair{!descriptor.paper, descriptor.slot};
    });
    return result;
}

bool CredentialStore::Delete(std::string_view slot, bool paper, std::string& error) {
    const std::wstring target = Target(slot, paper);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND) return true;
    error = "Windows Credential Manager error " + std::to_string(code);
    return false;
}

bool CredentialStore::Rename(std::string_view old_slot,
                             std::string_view new_slot, bool paper,
                             std::string& error) {
    if (old_slot.empty() || new_slot.empty()) {
        error = "Account name is required";
        return false;
    }
    if (old_slot == new_slot) return true;
    if (Exists(new_slot, paper)) {
        error = "An account with that name already exists";
        return false;
    }

    AlpacaCredentials credentials;
    if (!Load(old_slot, paper, credentials, error)) return false;
    if (!Save(new_slot, credentials, error)) return false;
    if (!Delete(old_slot, paper, error)) {
        error = "The account was copied to the new name, but the old "
                "credential could not be removed: " + error;
        return false;
    }
    return true;
}

bool CredentialStore::Save(const AlpacaCredentials& credentials, std::string& error) {
    return Save(credentials.paper ? "alpaca-paper-default" : "alpaca-live-default",
                credentials, error);
}

bool CredentialStore::Load(bool paper, AlpacaCredentials& credentials,
                           std::string& error) {
    return Load(paper ? "alpaca-paper-default" : "alpaca-live-default", paper,
                credentials, error);
}

bool CredentialStore::Delete(bool paper, std::string& error) {
    return Delete(paper ? "alpaca-paper-default" : "alpaca-live-default", paper,
                  error);
}
