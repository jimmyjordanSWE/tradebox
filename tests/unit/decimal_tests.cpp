#include "tradebox/core/decimal.h"

#include <gtest/gtest.h>

using tradebox::core::Decimal;

TEST(Decimal, PreservesFractionalPrecisionWithoutBinaryFloatingPoint) {
    const auto value = Decimal::Parse("000123.456789000");
    ASSERT_TRUE(value);
    EXPECT_EQ(value->ToString(), "123.456789");
}

TEST(Decimal, HandlesNineDecimalFractionalQuantities) {
    const auto one_share = Decimal::Parse("1");
    const auto fractional = Decimal::Parse("0.000000001");
    ASSERT_TRUE(one_share);
    ASSERT_TRUE(fractional);
    EXPECT_GT(*one_share, *fractional);
    EXPECT_EQ(fractional->ToString(), "0.000000001");
}

TEST(Decimal, ComparesArbitrarilyLargeValuesExactly) {
    const auto smaller =
        Decimal::Parse("999999999999999999999999999999.999999999");
    const auto larger =
        Decimal::Parse("1000000000000000000000000000000");
    ASSERT_TRUE(smaller);
    ASSERT_TRUE(larger);
    EXPECT_LT(*smaller, *larger);
}

TEST(Decimal, NormalizesSignedZero) {
    const auto value = Decimal::Parse("-0.0000");
    ASSERT_TRUE(value);
    EXPECT_EQ(value->ToString(), "0");
    EXPECT_TRUE(value->IsZero());
}

TEST(Decimal, RejectsMalformedInput) {
    EXPECT_FALSE(Decimal::Parse(""));
    EXPECT_FALSE(Decimal::Parse("."));
    EXPECT_FALSE(Decimal::Parse("1.2.3"));
    EXPECT_FALSE(Decimal::Parse("1e3"));
    EXPECT_FALSE(Decimal::Parse(" 1"));
}
