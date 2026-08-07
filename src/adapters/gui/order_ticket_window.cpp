#include "order_ticket_window.h"

#include "tradebox/application/trading_application.h"
#include "tradebox/core/order_request.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tradebox::gui {
namespace {

constexpr std::string_view kOrderTicketWindowId = "order-ticket.window";

std::pair<workstation::WindowInstanceState&, bool> EnsureWindow(
    workstation::WorkspaceState& state) {
    auto [found, inserted] = state.windows.try_emplace(
        std::string(kOrderTicketWindowId), workstation::WindowInstanceState{
            .id = std::string(kOrderTicketWindowId),
            .kind = "order-ticket",
            .title = "Order Ticket",
            .open = true,
            .bounds = {24.0f, 72.0f, 360.0f, 420.0f},
        });
    return {found->second, inserted};
}

constexpr std::array<std::string_view, 2> kSideOptions = {"buy", "sell"};
constexpr std::array<std::string_view, 5> kTypeOptions = {
    "market", "limit", "stop", "stop_limit", "trailing_stop"};
constexpr std::array<std::string_view, 6> kTifOptions = {
    "day", "gtc", "opg", "cls", "ioc", "fok"};

constexpr std::string_view kTypeLabels[] = {
    "Market", "Limit", "Stop", "Stop Limit", "Trailing Stop"};
constexpr std::string_view kTifLabels[] = {
    "Day", "GTC", "OPG", "CLS", "IOC", "FOK"};

int TypeIndex(std::string_view type) {
    for (std::size_t i = 0; i < kTypeOptions.size(); ++i)
        if (kTypeOptions[i] == type) return static_cast<int>(i);
    return 0;
}

int TifIndex(std::string_view tif) {
    for (std::size_t i = 0; i < kTifOptions.size(); ++i)
        if (kTifOptions[i] == tif) return static_cast<int>(i);
    return 0;
}

int SideIndex(std::string_view side) {
    for (std::size_t i = 0; i < kSideOptions.size(); ++i)
        if (kSideOptions[i] == side) return static_cast<int>(i);
    return 0;
}

void CopyString(std::array<char, 96>& destination,
                  const std::string& source) {
    const std::size_t count =
        std::min(source.size(), destination.size() - 1);
    std::copy_n(source.begin(), count, destination.begin());
    destination[count] = '\0';
}

std::string FormatPercent(float percent) {
    // Bracket sliders edit with "%.1f" precision; mirror that here so the
    // value carried in the order is exactly what the user saw.
    return std::format("{:.1f}", percent);
}

void DrawOrderResult(const std::optional<core::OrderCommandResult>& result) {
    if (!result) return;
    const bool accepted = result->AcceptedByBroker();
    const ImVec4 color = accepted
                             ? ImVec4{0.30f, 0.85f, 0.40f, 1.0f}
                             : ImVec4{0.95f, 0.32f, 0.32f, 1.0f};
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (accepted) {
        ImGui::TextUnformatted("Order accepted by broker");
        if (!result->broker_order_id.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(ID: %.*s)",
                                static_cast<int>(result->broker_order_id.size()),
                                result->broker_order_id.data());
        }
    } else {
        ImGui::TextUnformatted("Order rejected");
        if (!result->message.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(result->message.c_str());
        }
    }
    ImGui::PopStyleColor();
}

}  // namespace

void OrderTicketWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    [[maybe_unused]] const application::ApplicationUiSnapshot& snapshot) {
    auto [persisted, inserted] = EnsureWindow(state);
    if (inserted) persistent_changed_ = true;
    if (!persisted.open) return;

    ui::WorkspaceWindow window{
        .title = "Order Ticket",
        .id = std::string(kOrderTicketWindowId),
        .default_offset = {24.0f, 72.0f},
        .default_size = {360.0f, 420.0f},
        .open = true,
        .flags = ImGuiWindowFlags_NoCollapse,
    };
    workspace.ConstrainNextWindowSize({300.0f, 320.0f}, {500.0f, 600.0f});
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }

    // Find or create an order ticket for the selected symbol.
    const std::string& symbol = state.selected_symbol;
    auto ticket_it = std::ranges::find_if(
        state.order_tickets,
        [&](const workstation::OrderTicketState& t) {
            return t.symbol == symbol;
        });
    workstation::OrderTicketState* ticket = nullptr;
    if (ticket_it != state.order_tickets.end()) {
        ticket = &*ticket_it;
    } else {
        // Create a transient ticket for the selected symbol.
        // It will be persisted when the user modifies a field.
        state.order_tickets.push_back({
            .id = std::string(kOrderTicketWindowId) + "." + symbol,
            .symbol = symbol,
        });
        ticket = &state.order_tickets.back();
    }

    InteractionState& interaction = Interaction(*ticket);
    if (!interaction.initialized) {
        CopyString(interaction.amount, ticket->amount);
        CopyString(interaction.limit_price, ticket->limit_price);
        CopyString(interaction.stop_price, ticket->stop_price);
        interaction.initialized = true;
    }

    // Symbol display
    if (!symbol.empty()) {
        ImGui::TextUnformatted(symbol.c_str());
        ImGui::Separator();
    }

    // Side
    int side = SideIndex(ticket->side);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Side", &side, "Buy\0Sell\0")) {
        ticket->side = std::string(kSideOptions[static_cast<std::size_t>(side)]);
        persistent_changed_ = true;
    }

    // Order type
    int type = TypeIndex(ticket->type);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Type", &type,
                     "Market\0Limit\0Stop\0Stop Limit\0Trailing Stop\0")) {
        ticket->type = std::string(kTypeOptions[static_cast<std::size_t>(type)]);
        persistent_changed_ = true;
    }

    // Quantity / Notional
    bool is_notional = ticket->amount_is_notional;
    if (ImGui::Checkbox("Notional", &is_notional)) {
        ticket->amount_is_notional = is_notional;
        persistent_changed_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    std::string amount_label = is_notional ? "Notional $" : "Quantity";
    if (ImGui::InputText(amount_label.c_str(), interaction.amount.data(),
                         interaction.amount.size(),
                         ImGuiInputTextFlags_CharsDecimal |
                             ImGuiInputTextFlags_AutoSelectAll)) {
        ticket->amount = interaction.amount.data();
        persistent_changed_ = true;
    }

    // Limit price (for limit, stop_limit)
    if (type == 1 || type == 3) {
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputText("Limit Price", interaction.limit_price.data(),
                             interaction.limit_price.size(),
                             ImGuiInputTextFlags_CharsDecimal |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
            ticket->limit_price = interaction.limit_price.data();
            persistent_changed_ = true;
        }
    }

    // Stop price (for stop, stop_limit, trailing_stop)
    if (type >= 2) {
        const char* stop_label = (type == 4) ? "Trail $" : "Stop Price";
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputText(stop_label, interaction.stop_price.data(),
                             interaction.stop_price.size(),
                             ImGuiInputTextFlags_CharsDecimal |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
            ticket->stop_price = interaction.stop_price.data();
            persistent_changed_ = true;
        }
    }

    // Time in force
    int tif = TifIndex(ticket->time_in_force);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("TIF", &tif,
                     "Day\0GTC\0OPG\0CLS\0IOC\0FOK\0")) {
        ticket->time_in_force =
            std::string(kTifOptions[static_cast<std::size_t>(tif)]);
        persistent_changed_ = true;
    }

    // Extended hours
    bool extended = ticket->extended_hours;
    if (ImGui::Checkbox("Extended Hours", &extended)) {
        ticket->extended_hours = extended;
        persistent_changed_ = true;
    }

    // --- Bracket order section ---
    ImGui::Separator();
    auto bracket_it = state.bracket_drafts.find(symbol);
    if (bracket_it == state.bracket_drafts.end()) {
        // Insert a default bracket draft for this symbol.
        bracket_it = state.bracket_drafts
                         .try_emplace(symbol, workstation::BracketDraftState{})
                         .first;
    }
    workstation::BracketDraftState& bracket = bracket_it->second;

    bool bracket_enabled = bracket.target_percent > 0.0f ||
                           bracket.stop_percent > 0.0f;
    if (ImGui::Checkbox("Bracket (TP/SL)", &bracket_enabled)) {
        if (!bracket_enabled) {
            bracket.target_percent = 0.0f;
            bracket.stop_percent = 0.0f;
        } else {
            bracket.target_percent = 1.0f;
            bracket.stop_percent = 0.5f;
        }
        persistent_changed_ = true;
    }

    if (bracket_enabled) {
        ImGui::Indent(16.0f);

        float target = bracket.target_percent;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputFloat("Take Profit %", &target, 0.1f, 1.0f,
                              "%.1f%%")) {
            bracket.target_percent = std::max(0.0f, target);
            persistent_changed_ = true;
        }

        float stop = bracket.stop_percent;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputFloat("Stop Loss %", &stop, 0.1f, 1.0f,
                              "%.1f%%")) {
            bracket.stop_percent = std::max(0.0f, stop);
            persistent_changed_ = true;
        }

        bool gtc = bracket.gtc;
        if (ImGui::Checkbox("GTC Bracket Legs", &gtc)) {
            bracket.gtc = gtc;
            persistent_changed_ = true;
        }

        bool short_entry = bracket.short_entry;
        if (ImGui::Checkbox("Short Entry", &short_entry)) {
            bracket.short_entry = short_entry;
            persistent_changed_ = true;
        }

        ImGui::Unindent(16.0f);
    }

    ImGui::Separator();

    // Submit button
    const bool can_submit = !symbol.empty() && !ticket->amount.empty();
    if (!can_submit) ImGui::BeginDisabled();

    if (ImGui::Button("Submit Order", {140.0f, 32.0f})) {
        // Build the NativeOrderCommand from the ticket state.
        core::NativeOrderRequest request;
        request.symbol = ticket->symbol;
        request.side = (ticket->side == "sell")
                           ? core::OrderSide::Sell
                           : core::OrderSide::Buy;
        request.extended_hours = ticket->extended_hours;

        // Map type
        if (ticket->type == "market") {
            request.type = core::OrderType::Market;
        } else if (ticket->type == "limit") {
            request.type = core::OrderType::Limit;
        } else if (ticket->type == "stop") {
            request.type = core::OrderType::Stop;
        } else if (ticket->type == "stop_limit") {
            request.type = core::OrderType::StopLimit;
        } else if (ticket->type == "trailing_stop") {
            request.type = core::OrderType::TrailingStop;
        }

        // Map TIF
        if (ticket->time_in_force == "day") {
            request.time_in_force = core::TimeInForce::Day;
        } else if (ticket->time_in_force == "gtc") {
            request.time_in_force = core::TimeInForce::Gtc;
        } else if (ticket->time_in_force == "opg") {
            request.time_in_force = core::TimeInForce::Opg;
        } else if (ticket->time_in_force == "cls") {
            request.time_in_force = core::TimeInForce::Cls;
        } else if (ticket->time_in_force == "ioc") {
            request.time_in_force = core::TimeInForce::Ioc;
        } else if (ticket->time_in_force == "fok") {
            request.time_in_force = core::TimeInForce::Fok;
        }

        // Parse amount; leave the field unset when the text is not a valid
        // decimal so the order validator can report it.
        if (ticket->amount_is_notional) {
            if (auto parsed = core::Decimal::Parse(ticket->amount); parsed)
                request.notional = *parsed;
        } else {
            if (auto parsed = core::Decimal::Parse(ticket->amount); parsed)
                request.qty = *parsed;
        }

        // Parse prices
        if (auto parsed = core::Decimal::Parse(ticket->limit_price); parsed)
            request.limit_price = *parsed;
        if (auto parsed = core::Decimal::Parse(ticket->stop_price); parsed)
            request.stop_price = *parsed;

        // Attach bracket (take-profit / stop-loss) if enabled.
        if (bracket.target_percent > 0.0f || bracket.stop_percent > 0.0f) {
            request.order_class = core::OrderClass::Bracket;
            if (bracket.target_percent > 0.0f) {
                // The take-profit limit price is derived from the entry
                // direction. For a buy, TP is above; for a sell, TP is below.
                // The actual price computation requires a reference price
                // (e.g. last trade). We store the percentage and let the
                // broker adapter compute the absolute price.
                request.take_profit = core::TakeProfit{
                    .limit_price = *core::Decimal::Parse(
                        FormatPercent(bracket.target_percent)),
                };
            }
            if (bracket.stop_percent > 0.0f) {
                request.stop_loss = core::StopLoss{
                    .stop_price = *core::Decimal::Parse(
                        FormatPercent(bracket.stop_percent)),
                };
            }
        }

        // Validate
        auto errors = core::ValidateOrder(request);
        if (!errors.empty()) {
            // Show first error as a tooltip — the user can fix and retry.
            ImGui::OpenPopup("##order_validation_error");
            // Store the error message for display
            // (We'll show it in a popup below)
        } else {
            // Build the command
            core::PlaceOrderCommand place_cmd{
                .context = {
                    .source = "order-ticket",
                },
                .order = std::move(request),
            };
            // The actual submission happens through TradingApplication,
            // which is not directly accessible from the renderer.
            // The main loop will pick this up via a pending action.
            // For now, we open a confirmation popup.
            ImGui::OpenPopup("##order_confirmation");
        }
    }

    if (!can_submit) ImGui::EndDisabled();

    // Validation error popup
    if (ImGui::BeginPopupModal("##order_validation_error", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Order validation failed. Check your inputs.");
        if (ImGui::Button("OK", {80.0f, 0.0f}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Confirmation popup
    static bool confirmed = false;
    if (ImGui::BeginPopupModal("##order_confirmation", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Confirm Order");
        ImGui::Separator();
        ImGui::Text("Symbol: %s", ticket->symbol.c_str());
        ImGui::Text("Side: %s", ticket->side.c_str());
        ImGui::Text("Type: %s", ticket->type.c_str());
        ImGui::Text("Amount: %s %s", ticket->amount.c_str(),
                    ticket->amount_is_notional ? "(notional)" : "");
        if (!ticket->limit_price.empty())
            ImGui::Text("Limit: %s", ticket->limit_price.c_str());
        if (!ticket->stop_price.empty())
            ImGui::Text("Stop: %s", ticket->stop_price.c_str());
        ImGui::Text("TIF: %s", ticket->time_in_force.c_str());
        if (ticket->extended_hours)
            ImGui::TextUnformatted("Extended hours: Yes");
        if (bracket.target_percent > 0.0f || bracket.stop_percent > 0.0f) {
            ImGui::TextUnformatted("Bracket: Yes");
            if (bracket.target_percent > 0.0f)
                ImGui::Text("Take Profit: %.1f%%", bracket.target_percent);
            if (bracket.stop_percent > 0.0f)
                ImGui::Text("Stop Loss: %.1f%%", bracket.stop_percent);
            if (bracket.gtc)
                ImGui::TextUnformatted("Bracket legs: GTC");
            if (bracket.short_entry)
                ImGui::TextUnformatted("Short entry bracket");
        }
        ImGui::Separator();
        ImGui::Checkbox("I confirm this order", &confirmed);
        ImGui::BeginDisabled(!confirmed);
        if (ImGui::Button("Submit", {100.0f, 0.0f})) {
            // The actual submission is deferred to the main loop
            // via a pending action mechanism. For now, we just close.
            confirmed = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
            confirmed = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    workspace.EndWindow(window);
}

bool OrderTicketWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

OrderTicketWindowRenderer::InteractionState&
OrderTicketWindowRenderer::Interaction(
    const workstation::OrderTicketState& ticket) {
    return interactions_.try_emplace(ticket.id, InteractionState{}).first->second;
}

}  // namespace tradebox::gui