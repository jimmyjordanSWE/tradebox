#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace tradebox::workstation {

enum class WatchListColumnKind {
    Symbol,
    CurrentPrice,
    ChangeFromClose,
};

struct WatchListColumnDefinition {
    WatchListColumnKind kind;
    std::string_view id;
    std::string_view label;
};

inline constexpr std::string_view kWatchListTableId = "watch-list";

inline constexpr std::array<WatchListColumnDefinition, 3>
    kWatchListColumnDefinitions{{
        {WatchListColumnKind::Symbol, "symbol", "Ticker"},
        {WatchListColumnKind::CurrentPrice, "current_price", "Last Price"},
        {WatchListColumnKind::ChangeFromClose, "change_from_close",
         "Change from Close"},
    }};

[[nodiscard]] inline const WatchListColumnDefinition*
FindWatchListColumn(std::string_view id) {
    for (const auto& definition : kWatchListColumnDefinitions)
        if (definition.id == id) return &definition;
    return nullptr;
}

[[nodiscard]] inline const WatchListColumnDefinition*
FindWatchListColumn(WatchListColumnKind kind) {
    for (const auto& definition : kWatchListColumnDefinitions)
        if (definition.kind == kind) return &definition;
    return nullptr;
}

[[nodiscard]] inline std::optional<WatchListColumnKind>
WatchListColumnKindFromId(std::string_view id) {
    const auto* definition = FindWatchListColumn(id);
    if (definition == nullptr) return std::nullopt;
    return definition->kind;
}

}  // namespace tradebox::workstation
