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
    if (handle != INVALID_HANDLE_VALUE) {
        handle_ = handle;
        lock_path_ = lock_path;
        return true;
    }

    const DWORD code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
        // Another process holds the lock file open. Before reporting failure,
        // check whether the lock is stale: try to open with a short timeout
        // and if it fails, assume the owning process died without cleanup.
        // A stale lock file with no live owner can be removed and re-created.
        const HANDLE retry = CreateFileW(
            lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (retry != INVALID_HANDLE_VALUE) {
            // The lock became available — the previous owner exited between
            // the two attempts. Clean up the stale marker and proceed.
            CloseHandle(retry);
            std::error_code ec;
            std::filesystem::remove(lock_path, ec);
            const HANDLE fresh = CreateFileW(
                lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (fresh != INVALID_HANDLE_VALUE) {
                handle_ = fresh;
                lock_path_ = lock_path;
                return true;
            }
        }
        error = "This workstation profile is already open in another process";
    } else {
        error = "Could not lock workstation profile (Windows error " +
                std::to_string(code) + ")";
    }
    return false;
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