#include "tradebox/workstation/time_sales_documents.h"

#include "tradebox/workstation/stable_id.h"

#include <ranges>

namespace tradebox::workstation {

TimeSalesDocumentState* FindTimeSalesDocument(WorkspaceState& state,
                                              std::string_view id) {
    const auto found = std::ranges::find(state.time_sales, id,
                                         &TimeSalesDocumentState::id);
    return found == state.time_sales.end() ? nullptr : &*found;
}

std::expected<std::string, TimeSalesDocumentError>
CreateTimeSalesDocument(WorkspaceState& state) {
    std::string id;
    do { id = NewStableId("time-sales"); }
    while (state.windows.contains(id) || FindTimeSalesDocument(state, id));
    state.time_sales.push_back({.id = id});
    state.windows.emplace(id, WindowInstanceState{
        .id = id, .kind = "time-sales", .title = "Time & Sales", .open = true,
        .bounds = {96.0f, 120.0f, 760.0f, 440.0f}});
    return id;
}

std::expected<void, TimeSalesDocumentError> AssignTimeSalesInstrument(
    WorkspaceState& state, std::string_view id, std::string instrument_id,
    std::string symbol) {
    if (instrument_id.empty() || symbol.empty())
        return std::unexpected(TimeSalesDocumentError{"Time & Sales requires a stable instrument and symbol"});
    TimeSalesDocumentState* document = FindTimeSalesDocument(state, id);
    if (document == nullptr)
        return std::unexpected(TimeSalesDocumentError{"Time & Sales window does not exist"});
    document->instrument_id = std::move(instrument_id);
    document->symbol = std::move(symbol);
    document->ticker_input = document->symbol;
    state.windows.at(std::string(id)).title = document->symbol + " · Time & Sales";
    return {};
}

std::expected<void, TimeSalesDocumentError> ClearTimeSalesInstrument(
    WorkspaceState& state, std::string_view id) {
    TimeSalesDocumentState* document = FindTimeSalesDocument(state, id);
    if (document == nullptr)
        return std::unexpected(TimeSalesDocumentError{"Time & Sales window does not exist"});
    document->instrument_id.clear();
    document->symbol.clear();
    state.windows.at(std::string(id)).title = "Time & Sales";
    return {};
}

}  // namespace tradebox::workstation
