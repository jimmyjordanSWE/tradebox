#pragma once

#include <cstddef>
#include <cstdint>

namespace tradebox::core {

enum class TradeInitiation {
    Unknown,
    BuyerInitiated,
    SellerInitiated,
};

enum class TradeClassificationMethod {
    Unknown,
    QuoteTest,
    TickTest,
};

enum class TradePressureWeight {
    Trades,
    Shares,
    Notional,
};

struct TradePressureConfig {
    std::int64_t half_life_ms = 500;
    std::int64_t quote_max_age_ms = 2'000;
    std::int64_t tick_direction_max_age_ms = 5'000;
    std::int64_t correction_horizon_ms = 30'000;
    std::size_t maximum_recent_contributions = 512;
    TradePressureWeight weight = TradePressureWeight::Shares;
    // Activity is gross decayed directional weight divided by this scale
    // plus gross, which keeps activity bounded in [0, 1).
    double activity_saturation_weight = 100.0;
};

struct TradePressureSnapshot {
    double signed_pressure = 0.0;
    double activity = 0.0;
    double buyer_weight = 0.0;
    double seller_weight = 0.0;
    double unknown_weight = 0.0;
    std::uint64_t buyer_trades = 0;
    std::uint64_t seller_trades = 0;
    std::uint64_t unknown_trades = 0;
    TradeClassificationMethod latest_method =
        TradeClassificationMethod::Unknown;
    std::int64_t last_observation_ms = 0;
    std::int64_t half_life_ms = 500;
    bool stale = true;
};

}  // namespace tradebox::core
