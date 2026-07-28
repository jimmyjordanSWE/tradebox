#pragma once

#include <cstdint>
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
};

[[nodiscard]] std::vector<TradableAsset> SearchTradableAssets(
    const std::vector<TradableAsset>& assets, std::string query,
    std::size_t limit = 5);

}  // namespace tradebox::core
