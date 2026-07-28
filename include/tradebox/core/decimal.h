#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace tradebox::core {

struct DecimalError {
    std::string message;
};

class Decimal {
public:
    Decimal() = default;

    static std::expected<Decimal, DecimalError> Parse(std::string_view text);
    static Decimal Zero();

    std::string ToString() const;
    double ToDisplayDouble() const;
    bool IsZero() const;
    bool IsIntegral() const;
    std::uint32_t FractionalDigits() const;

    std::strong_ordering operator<=>(const Decimal& other) const;
    bool operator==(const Decimal& other) const;

private:
    Decimal(bool negative, std::string digits, std::uint32_t scale);
    std::strong_ordering CompareMagnitude(const Decimal& other) const;

    bool negative_ = false;
    std::string digits_ = "0";
    std::uint32_t scale_ = 0;
};

}  // namespace tradebox::core
