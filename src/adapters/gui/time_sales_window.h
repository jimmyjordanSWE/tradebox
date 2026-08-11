#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/core/market_data.h"
#include "tradebox/ui/workspace.h"

#include <future>
#include <map>
#include <string>
#include <vector>

namespace tradebox::gui {

struct TimeSalesHistoryRequest {
    std::string document_id;
    core::TickQuery query;
};

class TimeSalesWindowRenderer final {
public:
    void AppendSnapshotQuery(workstation::WorkspaceState& state,
                             application::UiSnapshotQuery& query);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] std::vector<TimeSalesHistoryRequest>
    ConsumeHistoryRequests();
    void SetHistoryRequest(std::string document_id,
                           std::future<core::TickSeries> request);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    struct HistoryState {
        std::string instrument_id;
        std::future<core::TickSeries> request;
        std::vector<core::MarketTrade> trades;
        bool requested = false;
    };

    void PollHistoryRequests();

    bool persistent_changed_ = false;
    std::map<std::string, HistoryState, std::less<>> history_;
    std::vector<TimeSalesHistoryRequest> history_requests_;
};

}  // namespace tradebox::gui
