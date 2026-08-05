#include "tradebox/workstation/profile_store.h"

#include "tradebox/workstation/profile_codec.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>

namespace tradebox::workstation {
namespace {

std::string ReadFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Could not open workstation profile: " + path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool FlushFile(const std::filesystem::path& path, std::string& error) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not flush workstation profile";
        return false;
    }
    const bool success = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!success) error = "Could not flush workstation profile";
    return success;
}

}  // namespace

std::filesystem::path ProfileStore::DefaultDirectory() {
    wchar_t* raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                                &raw_path);
    if (FAILED(result) || raw_path == nullptr)
        return std::filesystem::current_path() / "workspaces";
    const std::filesystem::path directory =
        std::filesystem::path(raw_path) / L"TradeBox" / L"workspaces";
    CoTaskMemFree(raw_path);
    return directory;
}

std::filesystem::path ProfileStore::DefaultProfilePath() {
    return DefaultDirectory() / L"Default.tbw";
}

std::vector<ProfileDescriptor> ProfileStore::Discover(
    const std::filesystem::path& directory) {
    std::vector<ProfileDescriptor> profiles;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return profiles;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != L".tbw")
            continue;
        std::string read_error;
        const std::string source = ReadFile(entry.path(), read_error);
        const auto state = DecodeProfile(source);
        if (!state) continue;
        ProfileLock probe;
        std::string lock_error;
        const bool available = probe.Acquire(entry.path(), lock_error);
        probe.Release();
        profiles.push_back({.path = entry.path(),
                            .id = state->profile.id,
                            .name = state->profile.name,
                            .locked = !available});
    }
    return profiles;
}

std::expected<WorkstationState, std::string> ProfileStore::Load(
    const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return WorkstationState::Defaults();
    std::string error;
    const std::string source = ReadFile(path, error);
    if (!error.empty()) return std::unexpected(error);
    return DecodeProfile(source);
}

bool ProfileStore::Open(const std::filesystem::path& path, bool read_only,
                        std::string& error) {
    Close();
    std::error_code directory_error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        error = "Could not create workstation profile directory: " +
                directory_error.message();
        return false;
    }
    if (!read_only && !lock_.Acquire(path, error)) return false;
    path_ = path;
    read_only_ = read_only;
    return true;
}

bool ProfileStore::WriteAtomic(const WorkstationState& state, std::string& error) {
    if (read_only_) {
        error = "Workstation profile is read-only";
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Could not create temporary workstation profile";
            return false;
        }
        stream << EncodeProfile(state);
        stream.flush();
        if (!stream) {
            error = "Could not write temporary workstation profile";
            return false;
        }
    }
    if (!FlushFile(temporary, error)) return false;
    if (MoveFileExW(temporary.c_str(), path_.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        error = "Could not atomically replace workstation profile (Windows error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

bool ProfileStore::SaveNow(const WorkstationState& state, std::string& error) {
    if (!IsOpen()) {
        error = "No workstation profile is open";
        return false;
    }
    if (!WriteAtomic(state, error)) return false;
    dirty_ = false;
    return true;
}

void ProfileStore::MarkDirty() {
    if (read_only_ || !IsOpen()) return;
    dirty_ = true;
    dirty_at_ = std::chrono::steady_clock::now();
}

bool ProfileStore::FlushIfDue(const WorkstationState& state, std::string& error) {
    if (!dirty_) return true;
    constexpr auto kSaveDelay = std::chrono::milliseconds(400);
    if (std::chrono::steady_clock::now() - dirty_at_ < kSaveDelay) return true;
    return SaveNow(state, error);
}

bool ProfileStore::Flush(const WorkstationState& state, std::string& error) {
    if (!dirty_) return true;
    return SaveNow(state, error);
}

void ProfileStore::Close() {
    path_.clear();
    read_only_ = false;
    dirty_ = false;
    lock_.Release();
}

}  // namespace tradebox::workstation
