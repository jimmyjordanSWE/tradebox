#pragma once

#include "tradebox/application/hotkey_order.h"
#include "tradebox/core/order_command.h"
#include "tradebox/ui/workspace.h"

#include <expected>
#include <future>
#include <optional>

namespace tradebox::gui {

struct HotkeyPreviewRequest {
    application::HotkeyBracketIntent long_intent;
    application::HotkeyBracketIntent short_intent;
};

class TradeHotkeyWindowRenderer final {
public:
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              workstation::ApplicationSettings& settings);
    [[nodiscard]] std::optional<HotkeyPreviewRequest> ConsumePreviewRequest();
    [[nodiscard]] std::optional<application::HotkeyBracketIntent>
    ConsumeSubmissionRequest();
    void SetPreviews(
        std::expected<application::HotkeyBracketPreview, std::string> long_preview,
        std::expected<application::HotkeyBracketPreview, std::string> short_preview);
    void SetSubmissionResult(std::future<core::OrderCommandResult> result);
    void SetSubmissionError(std::string error);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    bool persistent_changed_ = false;
    std::optional<HotkeyPreviewRequest> preview_request_;
    std::optional<application::HotkeyBracketIntent> active_intent_;
    std::optional<application::HotkeyBracketIntent> submission_request_;
    std::optional<application::HotkeyBracketPreview> long_preview_;
    std::optional<application::HotkeyBracketPreview> short_preview_;
    std::string long_preview_error_;
    std::string short_preview_error_;
    std::optional<std::future<core::OrderCommandResult>> pending_result_;
    std::optional<core::OrderCommandResult> result_;
    std::string error_;
};

}  // namespace tradebox::gui
