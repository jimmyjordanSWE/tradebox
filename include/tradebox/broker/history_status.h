#pragma once

#include "tradebox/core/bar_series.h"

#include <string>

namespace tradebox::broker {

enum class HistoryRequestState {
    Loading,
    Succeeded,
    Failed,
};

struct HistoryRequestStatus {
    core::BarSeriesKey key;
    core::BarRange range;
    HistoryRequestState state = HistoryRequestState::Loading;
    std::string message;
};

class IHistoryRequestStatusSink {
public:
    virtual ~IHistoryRequestStatusSink() = default;
    virtual void Publish(HistoryRequestStatus status) = 0;
};

}  // namespace tradebox::broker
