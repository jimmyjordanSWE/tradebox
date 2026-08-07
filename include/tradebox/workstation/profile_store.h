#pragma once

#include "tradebox/workstation/profile_lock.h"
#include "tradebox/workstation/state.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tradebox::workstation {

struct ProfileDescriptor {
    std::filesystem::path path;
    std::string id;
    std::string name;
    bool locked = false;
};

class ProfileStore {
public:
    ProfileStore() = default;
    ~ProfileStore() { Close(); }
    ProfileStore(const ProfileStore&) = delete;
    ProfileStore& operator=(const ProfileStore&) = delete;
    ProfileStore(ProfileStore&&) = delete;
    ProfileStore& operator=(ProfileStore&&) = delete;

    [[nodiscard]] static std::filesystem::path DefaultDirectory();
    [[nodiscard]] static std::filesystem::path DefaultProfilePath();
    [[nodiscard]] static std::vector<ProfileDescriptor> Discover(
        const std::filesystem::path& directory);

    [[nodiscard]] std::expected<WorkstationState, std::string> Load(
        const std::filesystem::path& path);
    [[nodiscard]] bool Open(const std::filesystem::path& path, bool read_only,
                            std::string& error);
    [[nodiscard]] bool SaveNow(const WorkstationState& state,
                               std::string& error);
    void MarkDirty();
    [[nodiscard]] bool FlushIfDue(const WorkstationState& state,
                                  std::string& error);
    [[nodiscard]] bool Flush(const WorkstationState& state,
                             std::string& error);
    void Close();

    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }
    [[nodiscard]] bool ReadOnly() const { return read_only_; }
    [[nodiscard]] bool IsOpen() const { return !path_.empty(); }

private:
    [[nodiscard]] bool WriteAtomic(const WorkstationState& state,
                                   std::string& error);

    std::filesystem::path path_;
    ProfileLock lock_;
    bool read_only_ = false;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point dirty_at_{};
};

}  // namespace tradebox::workstation

