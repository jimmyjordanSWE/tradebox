#include "tradebox/workstation/stable_id.h"

#include <array>
#include <iomanip>
#include <random>
#include <sstream>

namespace tradebox::workstation {

std::string NewStableId(std::string_view prefix) {
    std::random_device device;
    std::array<std::uint32_t, 4> words{};
    for (std::uint32_t& word : words) word = device();

    std::ostringstream output;
    if (!prefix.empty()) output << prefix << '.';
    output << std::hex << std::setfill('0')
           << std::setw(8) << words[0] << '-'
           << std::setw(4) << (words[1] & 0xffffU) << '-'
           << std::setw(4) << ((words[1] >> 16U) | 0x4000U) << '-'
           << std::setw(4) << ((words[2] & 0x3fffU) | 0x8000U) << '-'
           << std::setw(8) << words[3]
           << std::setw(4) << (words[2] >> 16U);
    return output.str();
}

}  // namespace tradebox::workstation
