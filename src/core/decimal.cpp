#include "tradebox/core/decimal.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace tradebox::core {
namespace {

void PadScale(std::string& digits, std::uint32_t current,
              std::uint32_t target) {
    digits.append(target - current, '0');
}

std::string AddMagnitude(std::string_view left,
                         std::string_view right) {
    const std::size_t width = std::max(left.size(), right.size());
    std::string result(width, '0');
    int carry = 0;
    for (std::size_t offset = 0; offset < width; ++offset) {
        const int l = offset < left.size()
                          ? left[left.size() - 1 - offset] - '0'
                          : 0;
        const int r = offset < right.size()
                          ? right[right.size() - 1 - offset] - '0'
                          : 0;
        const int sum = l + r + carry;
        result[width - 1 - offset] =
            static_cast<char>('0' + sum % 10);
        carry = sum / 10;
    }
    if (carry != 0)
        result.insert(result.begin(),
                      static_cast<char>('0' + carry));
    return result;
}

std::string SubtractMagnitude(std::string_view larger,
                              std::string_view smaller) {
    std::string result(larger);
    int borrow = 0;
    for (std::size_t offset = 0; offset < larger.size(); ++offset) {
        int digit = larger[larger.size() - 1 - offset] - '0' - borrow;
        const int subtract =
            offset < smaller.size()
                ? smaller[smaller.size() - 1 - offset] - '0'
                : 0;
        borrow = digit < subtract ? 1 : 0;
        if (borrow != 0) digit += 10;
        result[result.size() - 1 - offset] =
            static_cast<char>('0' + digit - subtract);
    }
    const auto first = result.find_first_not_of('0');
    return first == std::string::npos ? "0" : result.substr(first);
}

std::strong_ordering CompareDigits(
    std::string_view left, std::string_view right) {
    if (left.size() < right.size())
        return std::strong_ordering::less;
    if (left.size() > right.size())
        return std::strong_ordering::greater;
    if (left < right) return std::strong_ordering::less;
    if (left > right) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

void TrimLeadingZeros(std::string& digits) {
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) {
        digits = "0";
    } else if (first != 0) {
        digits.erase(0, first);
    }
}

std::string MultiplyMagnitude(
    std::string_view left, std::string_view right) {
    std::vector<unsigned int> result(
        left.size() + right.size(), 0);
    for (std::size_t left_offset = 0;
         left_offset < left.size(); ++left_offset) {
        const unsigned int left_digit =
            static_cast<unsigned int>(
                left[left.size() - 1 - left_offset] - '0');
        for (std::size_t right_offset = 0;
             right_offset < right.size(); ++right_offset) {
            const unsigned int right_digit =
                static_cast<unsigned int>(
                    right[right.size() - 1 - right_offset] -
                    '0');
            result[result.size() - 1 - left_offset -
                   right_offset] +=
                left_digit * right_digit;
        }
    }
    for (std::size_t index = result.size() - 1;
         index > 0; --index) {
        result[index - 1] += result[index] / 10;
        result[index] %= 10;
    }
    std::string digits;
    digits.reserve(result.size());
    for (const unsigned int digit : result)
        digits.push_back(
            static_cast<char>('0' + digit));
    TrimLeadingZeros(digits);
    return digits;
}

std::pair<std::string, std::string> DivideMagnitude(
    std::string_view numerator,
    std::string_view denominator) {
    std::string quotient;
    quotient.reserve(numerator.size());
    std::string remainder = "0";
    for (const char digit : numerator) {
        if (remainder == "0")
            remainder.assign(1, digit);
        else
            remainder.push_back(digit);
        TrimLeadingZeros(remainder);
        unsigned int quotient_digit = 0;
        while (CompareDigits(remainder, denominator) !=
               std::strong_ordering::less) {
            remainder =
                SubtractMagnitude(remainder, denominator);
            ++quotient_digit;
        }
        quotient.push_back(
            static_cast<char>('0' + quotient_digit));
    }
    TrimLeadingZeros(quotient);
    return {std::move(quotient), std::move(remainder)};
}

}  // namespace

Decimal::Decimal(bool negative, std::string digits, std::uint32_t scale,
                 double display_value)
    : negative_(negative),
      digits_(std::move(digits)),
      scale_(scale),
      display_value_(display_value) {}

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

    double display_value = 0;
    for (const char digit : digits)
        display_value = display_value * 10.0 +
                        static_cast<double>(digit - '0');
    for (std::uint32_t place = 0; place < scale; ++place)
        display_value /= 10.0;
    if (negative) display_value = -display_value;
    return Decimal(negative, std::move(digits), scale, display_value);
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
    return display_value_;
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

Decimal Decimal::operator+(const Decimal& other) const {
    const std::uint32_t scale = std::max(scale_, other.scale_);
    std::string left = digits_;
    std::string right = other.digits_;
    PadScale(left, scale_, scale);
    PadScale(right, other.scale_, scale);

    bool negative = false;
    std::string digits;
    if (negative_ == other.negative_) {
        negative = negative_;
        digits = AddMagnitude(left, right);
    } else if (left.size() > right.size() ||
               (left.size() == right.size() && left > right)) {
        negative = negative_;
        digits = SubtractMagnitude(left, right);
    } else if (right.size() > left.size() || right > left) {
        negative = other.negative_;
        digits = SubtractMagnitude(right, left);
    } else {
        return Decimal::Zero();
    }

    std::string text;
    if (negative) text.push_back('-');
    if (scale == 0) {
        text += digits;
    } else if (digits.size() <= scale) {
        text += "0.";
        text.append(scale - digits.size(), '0');
        text += digits;
    } else {
        const std::size_t point = digits.size() - scale;
        text.append(digits.substr(0, point));
        text.push_back('.');
        text.append(digits.substr(point));
    }
    return *Decimal::Parse(text);
}

Decimal& Decimal::operator+=(const Decimal& other) {
    if (!negative_ && !other.negative_ &&
        scale_ == other.scale_) {
        if (digits_.size() < other.digits_.size())
            digits_.insert(
                digits_.begin(),
                other.digits_.size() - digits_.size(), '0');
        int carry = 0;
        for (std::size_t offset = 0;
             offset < digits_.size(); ++offset) {
            const std::size_t left_index =
                digits_.size() - 1 - offset;
            const int right =
                offset < other.digits_.size()
                    ? other.digits_[other.digits_.size() -
                                    1 - offset] -
                          '0'
                    : 0;
            const int sum =
                digits_[left_index] - '0' + right + carry;
            digits_[left_index] =
                static_cast<char>('0' + sum % 10);
            carry = sum / 10;
        }
        if (carry != 0)
            digits_.insert(
                digits_.begin(),
                static_cast<char>('0' + carry));
        display_value_ += other.display_value_;
        return *this;
    }
    *this = *this + other;
    return *this;
}

Decimal Decimal::operator-(const Decimal& other) const {
    if (other.IsZero()) return *this;
    std::string negated = other.ToString();
    if (negated.front() == '-')
        negated.erase(negated.begin());
    else
        negated.insert(negated.begin(), '-');
    return *this + *Decimal::Parse(negated);
}

Decimal& Decimal::operator-=(const Decimal& other) {
    if (!negative_ && !other.negative_ &&
        scale_ == other.scale_ && *this >= other) {
        int borrow = 0;
        for (std::size_t offset = 0;
             offset < digits_.size(); ++offset) {
            const std::size_t left_index =
                digits_.size() - 1 - offset;
            int left =
                digits_[left_index] - '0' - borrow;
            const int right =
                offset < other.digits_.size()
                    ? other.digits_[other.digits_.size() -
                                    1 - offset] -
                          '0'
                    : 0;
            borrow = left < right ? 1 : 0;
            if (borrow != 0) left += 10;
            digits_[left_index] =
                static_cast<char>('0' + left - right);
        }
        const auto first = digits_.find_first_not_of('0');
        if (first == std::string::npos) {
            *this = Decimal::Zero();
            return *this;
        }
        if (first != 0) digits_.erase(0, first);
        while (scale_ > 0 && digits_.back() == '0') {
            digits_.pop_back();
            --scale_;
        }
        display_value_ -= other.display_value_;
        return *this;
    }
    *this = *this - other;
    return *this;
}

Decimal Decimal::operator*(const Decimal& other) const {
    if (IsZero() || other.IsZero()) return Decimal::Zero();
    std::string digits =
        MultiplyMagnitude(digits_, other.digits_);
    const std::uint64_t combined_scale =
        static_cast<std::uint64_t>(scale_) + other.scale_;
    std::string text;
    if (negative_ != other.negative_) text.push_back('-');
    if (combined_scale == 0) {
        text += digits;
    } else if (digits.size() <= combined_scale) {
        text += "0.";
        text.append(
            static_cast<std::size_t>(combined_scale) -
                digits.size(),
            '0');
        text += digits;
    } else {
        const std::size_t point =
            digits.size() -
            static_cast<std::size_t>(combined_scale);
        text.append(digits.substr(0, point));
        text.push_back('.');
        text.append(digits.substr(point));
    }
    return *Decimal::Parse(text);
}

std::expected<Decimal, DecimalError> Decimal::Divide(
    const Decimal& other,
    std::uint32_t fractional_digits) const {
    if (other.IsZero())
        return std::unexpected(
            DecimalError{"cannot divide by zero"});
    if (IsZero()) return Decimal::Zero();

    std::string numerator = digits_;
    numerator.append(
        static_cast<std::size_t>(fractional_digits) +
            other.scale_,
        '0');
    std::string denominator = other.digits_;
    denominator.append(scale_, '0');
    auto [digits, remainder] =
        DivideMagnitude(numerator, denominator);
    if (CompareDigits(
            AddMagnitude(remainder, remainder),
            denominator) != std::strong_ordering::less)
        digits = AddMagnitude(digits, "1");
    std::string text;
    if (negative_ != other.negative_) text.push_back('-');
    if (fractional_digits == 0) {
        text += digits;
    } else if (digits.size() <= fractional_digits) {
        text += "0.";
        text.append(fractional_digits - digits.size(), '0');
        text += digits;
    } else {
        const std::size_t point =
            digits.size() - fractional_digits;
        text.append(digits.substr(0, point));
        text.push_back('.');
        text.append(digits.substr(point));
    }
    return Decimal::Parse(text);
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
