#include "time_sales_window.h"

#include "tradebox/workstation/asset_preferences.h"
#include "tradebox/workstation/time_sales_documents.h"

#include "gui_controls.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <format>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace tradebox::gui {
namespace {

constexpr std::array<TableColumnChoice, 9> kColumns{{
    {"timestamp", "Timestamp", false}, {"time", "Time", false},
    {"minute", "Minute", false}, {"price", "Price", false},
    {"size", "Size", false}, {"exchange", "Exchange", false},
    {"conditions", "Conditions", false}, {"tape", "Tape", false},
    {"trade_id", "Trade ID", false},
}};
constexpr std::int64_t kInitialHistoryLookbackNs = 15LL * 60LL * 1'000'000'000LL;
constexpr std::size_t kMaximumDisplayedTrades = 2'000;

std::string Time(std::int64_t ns, const char* pattern) {
    if (ns <= 0) return "--";
    const std::time_t seconds = static_cast<std::time_t>(ns / 1'000'000'000);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::ostringstream output;
    output << std::put_time(&local, pattern);
    return output.str();
}

std::string Conditions(const core::MarketTrade& trade) {
    std::string result;
    for (const std::string& condition : trade.conditions) {
        if (!result.empty()) result += ' ';
        result += condition;
    }
    return result;
}

const application::UiAssetSearchResult* MatchesFor(
    const application::ApplicationUiSnapshot& snapshot, std::string_view query) {
    const auto found = std::ranges::find(snapshot.asset_search_results, query,
                                         &application::UiAssetSearchResult::query);
    return found == snapshot.asset_search_results.end() ? nullptr : &*found;
}

bool SameSymbol(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    return std::ranges::equal(left, right, [](char lhs, char rhs) {
        return std::toupper(static_cast<unsigned char>(lhs)) ==
               std::toupper(static_cast<unsigned char>(rhs));
    });
}

std::vector<const core::MarketTrade*> DisplayTrades(
    const std::vector<core::MarketTrade>& history,
    const core::MarketDataSnapshot* market) {
    std::vector<const core::MarketTrade*> result;
    result.reserve(history.size() + (market == nullptr ? 0 : market->trades.size()));
    for (const core::MarketTrade& trade : history) result.push_back(&trade);
    if (market != nullptr)
        for (const core::MarketTrade& trade : market->trades) result.push_back(&trade);
    std::ranges::sort(result, std::greater<>{},
                      [](const core::MarketTrade* trade) { return trade->event_time_ns; });
    std::vector<const core::MarketTrade*> unique;
    unique.reserve(std::min(result.size(), kMaximumDisplayedTrades));
    std::unordered_set<std::string> seen;
    for (const core::MarketTrade* trade : result) {
        if (!trade->trade_id.empty() &&
            !seen.insert(trade->trade_id + ":" +
                         std::to_string(trade->event_time_ns)).second)
            continue;
        unique.push_back(trade);
        if (unique.size() == kMaximumDisplayedTrades) break;
    }
    return unique;
}

void EnsureColumns(workstation::PersistentTableState& table) {
    if (!table.columns.empty()) return;
    constexpr std::array<std::string_view, 6> defaults{
        "time", "price", "size", "exchange", "conditions", "tape"};
    for (std::size_t index = 0; index < defaults.size(); ++index)
        table.columns.push_back({.id = std::string(defaults[index]),
                                 .order = static_cast<int>(index),
                                 .width = 110.0f, .visible = true});
}

}  // namespace

void TimeSalesWindowRenderer::AppendSnapshotQuery(
    workstation::WorkspaceState& state, application::UiSnapshotQuery& query) {
    for (const workstation::TimeSalesDocumentState& document : state.time_sales) {
        const auto window = state.windows.find(document.id);
        if (window == state.windows.end() || !window->second.open) continue;
        if (!document.symbol.empty()) {
            query.market_symbols.push_back(document.symbol);
            HistoryState& history = history_[document.id];
            if (history.instrument_id != document.instrument_id) {
                history = HistoryState{.instrument_id = document.instrument_id};
            }
            if (!history.requested && query.as_of_ns > kInitialHistoryLookbackNs) {
                history.requested = true;
                history_requests_.push_back({
                    .document_id = document.id,
                    .query = {.instrument_id = document.instrument_id,
                              .symbol = document.symbol,
                              .start_ns = query.as_of_ns - kInitialHistoryLookbackNs,
                              .end_ns = query.as_of_ns,
                              .include_trades = true,
                              .include_quotes = false},
                });
            }
        }
        else if (!document.ticker_input.empty()) query.asset_searches.push_back(document.ticker_input);
    }
    if (!query.asset_searches.empty()) query.asset_limit = std::max(query.asset_limit, std::size_t{12});
}

void TimeSalesWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    PollHistoryRequests();
    EnsureColumns(state.time_sales_table);
    for (workstation::TimeSalesDocumentState& document : state.time_sales) {
        const auto found = state.windows.find(document.id);
        if (found == state.windows.end() || !found->second.open) continue;
        ui::WorkspaceWindow window{.title = found->second.title, .id = document.id,
                                   .default_offset = {96, 120}, .default_size = {760, 440}};
        workspace.ConstrainNextWindowSize({420, 240});
        if (!workspace.BeginWindow(window)) { workspace.EndWindow(window); continue; }
        ImGui::PushID(document.id.c_str());
        std::array<char, 32> ticker{};
        std::copy_n(document.ticker_input.data(),
                    std::min(document.ticker_input.size(), ticker.size() - 1U), ticker.data());
        ImGui::SetNextItemWidth(140.0f);
        const bool ticker_edited = ImGui::InputText(
            "Ticker", ticker.data(), ticker.size(), ImGuiInputTextFlags_CharsUppercase);
        const bool ticker_submitted = ImGui::IsItemFocused() &&
                                      ImGui::IsKeyPressed(ImGuiKey_Enter);
        if (ticker_edited) {
            document.ticker_input = ticker.data();
            document.instrument_id.clear(); document.symbol.clear();
            persistent_changed_ = true;
        }
        if (!document.instrument_id.empty() && ImGui::Button("Change")) {
            static_cast<void>(workstation::ClearTimeSalesInstrument(state, document.id));
            persistent_changed_ = true;
        }
        if (document.instrument_id.empty()) {
            if (const auto* matches = MatchesFor(snapshot, document.ticker_input)) {
                const core::TradableAsset* submitted_asset = nullptr;
                if (ticker_submitted && !matches->matches.empty()) {
                    const auto exact = std::ranges::find_if(
                        matches->matches, [&document](const auto& asset) {
                            return SameSymbol(asset.symbol, document.ticker_input);
                        });
                    submitted_asset = exact != matches->matches.end()
                                          ? &*exact : &matches->matches.front();
                }
                for (const core::TradableAsset& asset : matches->matches) {
                    const std::string label = asset.symbol + " - " + asset.name;
                    if ((ImGui::Selectable(label.c_str()) || &asset == submitted_asset) &&
                        workstation::AssignTimeSalesInstrument(state, document.id,
                                                              asset.instrument_id, asset.symbol)) {
                        workstation::RecordAssetSelection(state, asset.instrument_id);
                        persistent_changed_ = true;
                    }
                }
            }
            ImGui::TextDisabled("Choose an instrument to view live trades.");
        } else {
            ImGui::SameLine(); ImGui::TextDisabled("%s", document.symbol.c_str());
            const auto columns = OrderedVisibleTableColumns(state.time_sales_table);
            const TableColumnActions actions = DrawTableColumnControls(
                state.time_sales_table, kColumns, "time_sales_columns");
            if (actions.add) {
                const auto choice = std::ranges::find(kColumns, *actions.add,
                                                      &TableColumnChoice::id);
                if (choice != kColumns.end()) {
                    const int order = static_cast<int>(state.time_sales_table.columns.size());
                    state.time_sales_table.columns.push_back({.id = std::string(choice->id), .order = order,
                                                              .width = 110.0f, .visible = true});
                    persistent_changed_ = true;
                }
            }
            if (actions.remove) {
                const auto old_size = state.time_sales_table.columns.size();
                std::erase_if(state.time_sales_table.columns, [&](const auto& column) {
                    return column.id == *actions.remove;
                });
                persistent_changed_ = persistent_changed_ ||
                                      old_size != state.time_sales_table.columns.size();
            }
            const HistoryState& history = history_[document.id];
            const core::MarketDataSnapshot* market = snapshot.markets.Find(document.instrument_id);
            const auto trades = DisplayTrades(history.trades, market);
            if (history.request.valid()) ImGui::TextDisabled("Loading recent trades...");
            else if (ImGui::Button("Refresh recent")) {
                history_.erase(document.id);
            }
            if (ImGui::BeginTable("trades", static_cast<int>(columns.size()),
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_NoSavedSettings, {0, 0})) {
                SetupPersistentTableColumns(columns, kColumns, 110.0f);
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(trades.size()));
                while (clipper.Step()) {
                    for (int index = clipper.DisplayStart;
                         index < clipper.DisplayEnd; ++index) {
                    const core::MarketTrade* trade_ptr =
                        trades[static_cast<std::size_t>(index)];
                    const core::MarketTrade& trade = *trade_ptr;
                    ImGui::TableNextRow();
                    for (const auto* column : columns) { ImGui::TableNextColumn();
                        const std::string& id = column->id;
                        const std::string value = id == "timestamp" ? Time(trade.event_time_ns, "%Y-%m-%d %H:%M:%S") :
                            id == "time" ? Time(trade.event_time_ns, "%H:%M:%S") :
                            id == "minute" ? Time(trade.event_time_ns, "%H:%M") :
                            id == "price" ? std::format("${:.2f}", trade.price.ToDisplayDouble()) :
                            id == "size" ? trade.size.ToString() : id == "exchange" ? trade.exchange :
                            id == "conditions" ? Conditions(trade) : id == "tape" ? trade.tape : trade.trade_id;
                        ImGui::TextUnformatted(value.c_str());
                    }
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::PopID(); workspace.EndWindow(window);
    }
}

std::vector<TimeSalesHistoryRequest>
TimeSalesWindowRenderer::ConsumeHistoryRequests() {
    return std::exchange(history_requests_, {});
}

void TimeSalesWindowRenderer::SetHistoryRequest(
    std::string document_id, std::future<core::TickSeries> request) {
    history_[document_id].request = std::move(request);
}

void TimeSalesWindowRenderer::PollHistoryRequests() {
    for (auto& entry : history_) {
        HistoryState& history = entry.second;
        if (!history.request.valid() ||
            history.request.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
            continue;
        try {
            core::TickSeries series = history.request.get();
            history.trades = std::move(series.trades);
        } catch (const std::exception&) {}
    }
}

bool TimeSalesWindowRenderer::ConsumePersistentChanges() {
    return std::exchange(persistent_changed_, false);
}

}  // namespace tradebox::gui
