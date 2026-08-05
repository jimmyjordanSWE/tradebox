#include "tradebox/workstation/profile_codec.h"
#include "tradebox/workstation/profile_store.h"
#include "tradebox/workstation/validation.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>

namespace tradebox::workstation {
namespace {

class TemporaryProfileDirectory {
public:
    TemporaryProfileDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                "tradebox-workstation-state-tests" /
                std::to_string(::GetCurrentProcessId());
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryProfileDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] std::filesystem::path Profile(std::string_view name) const {
        return path_ / (std::string(name) + ".tbw");
    }

private:
    std::filesystem::path path_;
};

TEST(WorkstationState, DefaultsAreValidAndStable) {
    WorkstationState state = WorkstationState::Defaults();
    std::string error;
    EXPECT_TRUE(ValidateAndNormalize(state, error)) << error;
    EXPECT_EQ(state.profile.schema_version, kCurrentSchemaVersion);
    EXPECT_FALSE(state.profile.id.empty());
    EXPECT_TRUE(state.workspace.windows.empty());
    EXPECT_TRUE(state.workspace.charts.empty());
    EXPECT_TRUE(state.workspace.order_tickets.empty());
}

TEST(WorkstationState, ProfileRoundTripPreservesSemanticState) {
    WorkstationState source = WorkstationState::Defaults();
    source.profile.name = "Research";
    source.workspace.selected_symbol = "NVDA";
    source.workspace.windows.emplace(
        "tool.activity",
        WindowInstanceState{.id = "tool.activity",
                            .kind = "tool",
                            .title = "ACTIVITY",
                            .open = true,
                            .bounds = {40.0f, 40.0f, 900.0f, 600.0f},
                            .selected_tab = "Positions"});
    source.workspace.windows.at("tool.activity").selected_tab = "Orders";
    source.workspace.windows.at("tool.activity").tables["orders"].columns = {
        {.id = "symbol", .order = 2, .width = 180.0f, .visible = true,
         .sort_direction = "ascending"},
    };
    source.workspace.charts.push_back({.id = "chart:1", .instrument_id = "alpaca:1",
                                       .symbol = "NVDA", .timeframe = "5Min",
                                       .visible_bars = 240});
    const std::string encoded = EncodeProfile(source);
    const auto decoded = DecodeProfile(encoded);
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(decoded->profile.name, "Research");
    EXPECT_EQ(decoded->workspace.selected_symbol, "NVDA");
    EXPECT_EQ(decoded->workspace.windows.at("tool.activity").selected_tab,
              "Orders");
    ASSERT_EQ(decoded->workspace.charts.size(), 1U);
    EXPECT_EQ(decoded->workspace.charts.front().timeframe, "5Min");
    EXPECT_EQ(EncodeProfile(*decoded), encoded);
}

TEST(WorkstationState, StoreRoundTripsThroughOneProfileFile) {
    TemporaryProfileDirectory temporary;
    const std::filesystem::path path = temporary.Profile("research");
    ProfileStore writer;
    std::string error;
    ASSERT_TRUE(writer.Open(path, false, error)) << error;
    WorkstationState state = WorkstationState::Defaults();
    state.profile.name = "Research";
    ASSERT_TRUE(writer.SaveNow(state, error)) << error;
    writer.Close();

    ProfileStore reader;
    ASSERT_TRUE(reader.Open(path, true, error)) << error;
    const auto restored = reader.Load(path);
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(restored->profile.name, "Research");
    reader.Close();
}

TEST(WorkstationState, RejectsDuplicateDocumentIdentity) {
    WorkstationState state = WorkstationState::Defaults();
    state.workspace.charts = {
        {.id = "chart:1", .symbol = "AMD"},
        {.id = "chart:1", .symbol = "AAPL"},
    };
    std::string error;
    EXPECT_FALSE(ValidateAndNormalize(state, error));
    EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace tradebox::workstation
