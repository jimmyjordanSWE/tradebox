#include "tradebox/platform/credentials.h"

#include <windows.h>
#include <wincred.h>

#include <vector>
#include <utility>

namespace {

std::wstring Target(bool paper) {
    return paper ? L"TradeBoxNative/Alpaca/Paper" : L"TradeBoxNative/Alpaca/Live";
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

}  // namespace

bool CredentialStore::Save(const AlpacaCredentials& credentials, std::string& error) {
    const std::wstring target = Target(credentials.paper);
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

bool CredentialStore::Load(bool paper, AlpacaCredentials& credentials,
                           std::string& error) {
    PCREDENTIALW value = nullptr;
    const std::wstring target = Target(paper);
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

bool CredentialStore::Delete(bool paper, std::string& error) {
    const std::wstring target = Target(paper);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND) return true;
    error = "Windows Credential Manager error " + std::to_string(code);
    return false;
}
