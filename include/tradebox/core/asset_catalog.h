#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tradebox::core {

struct TradableAsset {
    std::string symbol;
    std::string name;
    std::string exchange;
    bool active = false;
    bool tradable = false;
    bool shortable = false;
    bool fractionable = false;
    std::int64_t previous_volume = 0;
    std::int64_t previous_dollar_volume = 0;
    std::int64_t received_at_ms = 0;
    // Domain identity is opaque and stable. External identifiers and symbols
    // are aliases that may be enriched or changed independently.
    std::string instrument_id;
    std::string isin;
    std::string cusip;
    std::string sedol;
    std::string provider_asset_id;
};

[[nodiscard]] std::vector<TradableAsset> SearchTradableAssets(
    const std::vector<TradableAsset>& assets, std::string query,
    std::size_t limit = 5,
    std::span<const std::string> preferred_instrument_ids = {});

}  // namespace tradebox::core
