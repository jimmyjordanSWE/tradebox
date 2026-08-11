#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/core/order_command.h"
#include "tradebox/core/order_request.h"
#include "tradebox/ui/workspace.h"

#include <array>
#include <future>
#include <string>
#include <optional>

namespace tradebox::gui {

class PositionsWindowRenderer final {
public:
    void AppendSnapshotQuery(workstation::WorkspaceState& state,
                             application::UiSnapshotQuery& query);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] std::optional<std::string> ConsumeExitRequest();
    void SetExitResult(std::future<core::OrderCommandResult> result);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    std::optional<std::string> exit_confirmation_symbol_;
    std::optional<std::string> exit_request_;
    std::optional<std::future<core::OrderCommandResult>> pending_exit_result_;
    std::optional<core::OrderCommandResult> exit_result_;
    bool persistent_changed_ = false;
};

class OrdersWindowRenderer final {
public:
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    struct ActionRequest {
        enum class Kind { Cancel, Replace };

        Kind kind = Kind::Cancel;
        std::string order_id;
        core::ReplaceOrderRequest replacement;
    };

    [[nodiscard]] std::optional<ActionRequest> ConsumeActionRequest();
    void SetSubmissionResult(std::future<core::OrderCommandResult> result);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    struct ReplaceDraft {
        std::string order_id;
        std::string symbol;
        bool change_qty = false;
        bool change_notional = false;
        bool change_limit_price = false;
        bool change_stop_price = false;
        bool change_trail = false;
        bool change_time_in_force = false;
        int time_in_force_index = 0;
        std::array<char, 64> qty{};
        std::array<char, 64> notional{};
        std::array<char, 64> limit_price{};
        std::array<char, 64> stop_price{};
        std::array<char, 64> trail{};
        std::string error;
    };

    std::optional<std::string> cancel_confirmation_order_id_;
    std::optional<ReplaceDraft> replace_draft_;
    std::optional<ActionRequest> action_request_;
    std::optional<std::future<core::OrderCommandResult>> pending_result_;
    std::optional<core::OrderCommandResult> result_;
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
