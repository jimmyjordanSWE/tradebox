#pragma once

#include <filesystem>
#include <string>

namespace tradebox::workstation {

class ProfileLock {
public:
    ProfileLock() = default;
    ~ProfileLock();
    ProfileLock(const ProfileLock&) = delete;
    ProfileLock& operator=(const ProfileLock&) = delete;
    ProfileLock(ProfileLock&& other) noexcept;
    ProfileLock& operator=(ProfileLock&& other) noexcept;

    [[nodiscard]] bool Acquire(const std::filesystem::path& profile_path,
                               std::string& error);
    void Release();
    [[nodiscard]] bool Held() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
    std::filesystem::path lock_path_;
};

}  // namespace tradebox::workstation

