#include "tradebox/workstation/profile_lock.h"

#include <windows.h>

#include <utility>

namespace tradebox::workstation {

ProfileLock::~ProfileLock() { Release(); }

ProfileLock::ProfileLock(ProfileLock&& other) noexcept
    : handle_(other.handle_), lock_path_(std::move(other.lock_path_)) {
    other.handle_ = nullptr;
}

ProfileLock& ProfileLock::operator=(ProfileLock&& other) noexcept {
    if (this == &other) return *this;
    Release();
    handle_ = other.handle_;
    lock_path_ = std::move(other.lock_path_);
    other.handle_ = nullptr;
    return *this;
}

bool ProfileLock::Acquire(const std::filesystem::path& profile_path,
                          std::string& error) {
    Release();
    const std::filesystem::path lock_path = profile_path.wstring() + L".lock";
    const HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      0, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
            error = "This workstation profile is already open in another process";
        else
            error = "Could not lock workstation profile (Windows error " +
                    std::to_string(code) + ")";
        return false;
    }
    handle_ = handle;
    lock_path_ = lock_path;
    return true;
}

void ProfileLock::Release() {
    if (handle_ == nullptr) return;
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    std::error_code error;
    std::filesystem::remove(lock_path_, error);
    lock_path_.clear();
}

}  // namespace tradebox::workstation

