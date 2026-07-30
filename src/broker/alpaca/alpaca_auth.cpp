#include "tradebox/broker/alpaca_auth.h"

namespace tradebox::broker::alpaca {
namespace {

void AppendJsonString(
    std::string& output, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output.push_back(kHex[character >> 4]);
                output.push_back(kHex[character & 0x0F]);
            } else {
                output.push_back(
                    static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

}  // namespace

std::string BuildAuthenticationMessage(
    std::string_view api_key, std::string_view api_secret) {
    std::string message =
        R"({"action":"auth","key":)";
    AppendJsonString(message, api_key);
    message += R"(,"secret":)";
    AppendJsonString(message, api_secret);
    message.push_back('}');
    return message;
}

}  // namespace tradebox::broker::alpaca
