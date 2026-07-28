#include "tradebox/core/decimal.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace tradebox::core {

Decimal::Decimal(bool negative, std::string digits, std::uint32_t scale)
    : negative_(negative), digits_(std::move(digits)), scale_(scale) {}

std::expected<Decimal, DecimalError> Decimal::Parse(std::string_view text) {
    if (text.empty())
        return std::unexpected(DecimalError{"decimal is empty"});

    std::size_t index = 0;
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        index = 1;
    }
    if (index == text.size())
        return std::unexpected(DecimalError{"decimal has no digits"});

    std::string digits;
    digits.reserve(text.size());
    bool decimal_point_seen = false;
    std::uint32_t scale = 0;
    bool digit_seen = false;

    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '.') {
            if (decimal_point_seen)
                return std::unexpected(
                    DecimalError{"decimal has multiple points"});
            decimal_point_seen = true;
            continue;
        }
        if (character < '0' || character > '9')
            return std::unexpected(
                DecimalError{"decimal contains an invalid character"});
        digit_seen = true;
        digits.push_back(character);
        if (decimal_point_seen) {
            if (scale == std::numeric_limits<std::uint32_t>::max())
                return std::unexpected(DecimalError{"decimal scale overflow"});
            ++scale;
        }
    }

    if (!digit_seen)
        return std::unexpected(DecimalError{"decimal has no digits"});

    const auto first_non_zero = digits.find_first_not_of('0');
    if (first_non_zero == std::string::npos) return Decimal::Zero();
    digits.erase(0, first_non_zero);

    while (scale > 0 && digits.back() == '0') {
        digits.pop_back();
        --scale;
    }

    return Decimal(negative, std::move(digits), scale);
}

Decimal Decimal::Zero() {
    return {};
}

std::string Decimal::ToString() const {
    if (IsZero()) return "0";

    std::string result;
    if (negative_) result.push_back('-');
    if (scale_ == 0) {
        result += digits_;
        return result;
    }

    if (digits_.size() <= scale_) {
        result += "0.";
        result.append(scale_ - digits_.size(), '0');
        result += digits_;
        return result;
    }

    const std::size_t point_at = digits_.size() - scale_;
    result.append(digits_.substr(0, point_at));
    result.push_back('.');
    result.append(digits_.substr(point_at));
    return result;
}

double Decimal::ToDisplayDouble() const {
    const std::string value = ToString();
    return std::strtod(value.c_str(), nullptr);
}

bool Decimal::IsZero() const {
    return digits_ == "0";
}

bool Decimal::IsIntegral() const {
    return scale_ == 0;
}

std::uint32_t Decimal::FractionalDigits() const {
    return scale_;
}

std::strong_ordering Decimal::operator<=>(const Decimal& other) const {
    if (IsZero() && other.IsZero()) return std::strong_ordering::equal;
    if (IsZero())
        return other.negative_ ? std::strong_ordering::greater
                               : std::strong_ordering::less;
    if (other.IsZero())
        return negative_ ? std::strong_ordering::less
                         : std::strong_ordering::greater;
    if (negative_ != other.negative_)
        return negative_ ? std::strong_ordering::less
                         : std::strong_ordering::greater;
    const std::strong_ordering magnitude = CompareMagnitude(other);
    if (!negative_) return magnitude;
    if (magnitude == std::strong_ordering::less)
        return std::strong_ordering::greater;
    if (magnitude == std::strong_ordering::greater)
        return std::strong_ordering::less;
    return std::strong_ordering::equal;
}

bool Decimal::operator==(const Decimal& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
}

std::strong_ordering Decimal::CompareMagnitude(const Decimal& other) const {
    const auto exponent = [](const Decimal& value) {
        return static_cast<std::int64_t>(value.digits_.size()) -
               static_cast<std::int64_t>(value.scale_);
    };
    const std::int64_t left_exponent = exponent(*this);
    const std::int64_t right_exponent = exponent(other);
    if (left_exponent < right_exponent) return std::strong_ordering::less;
    if (left_exponent > right_exponent) return std::strong_ordering::greater;

    const std::size_t width = std::max(digits_.size(), other.digits_.size());
    for (std::size_t index = 0; index < width; ++index) {
        const char left = index < digits_.size() ? digits_[index] : '0';
        const char right =
            index < other.digits_.size() ? other.digits_[index] : '0';
        if (left < right) return std::strong_ordering::less;
        if (left > right) return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

}  // namespace tradebox::core
