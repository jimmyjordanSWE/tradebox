#pragma once

#include <string>
#include <string_view>

namespace tradebox::workstation {

// Generates an opaque stable semantic ID for persisted workstation entities.
// Visible labels, symbols, and collection indexes must never be used as IDs.
[[nodiscard]] std::string NewStableId(std::string_view prefix);

}  // namespace tradebox::workstation
