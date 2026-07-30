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

TEST(Decimal, AddsExactValuesAcrossScalesAndSigns) {
    const Decimal left = *Decimal::Parse("123.456789001");
    const Decimal right = *Decimal::Parse("0.000000009");
    EXPECT_EQ((left + right).ToString(), "123.45678901");
    EXPECT_EQ(
        (left + *Decimal::Parse("-23.456789001")).ToString(),
        "100");
    EXPECT_EQ(
        (*Decimal::Parse("-2.5") + *Decimal::Parse("1.25"))
            .ToString(),
        "-1.25");
}

TEST(Decimal, SubtractsExactNonNegativeValuesInPlace) {
    Decimal volume = *Decimal::Parse("1000000.75");
    volume -= *Decimal::Parse("999999.5");
    EXPECT_EQ(volume.ToString(), "1.25");
    volume -= *Decimal::Parse("1.25");
    EXPECT_EQ(volume.ToString(), "0");
}

TEST(Decimal, MultipliesAndDividesWithoutBinaryFloatingPoint) {
    const Decimal quantity = *Decimal::Parse("-2.5");
    const Decimal price = *Decimal::Parse("101.125");
    EXPECT_EQ((quantity * price).ToString(), "-252.8125");

    const auto ratio =
        Decimal::Parse("1")->Divide(*Decimal::Parse("3"));
    ASSERT_TRUE(ratio);
    EXPECT_EQ(ratio->ToString(), "0.333333333");

    const auto rounded =
        Decimal::Parse("2")->Divide(*Decimal::Parse("3"), 2);
    ASSERT_TRUE(rounded);
    EXPECT_EQ(rounded->ToString(), "0.67");
    EXPECT_FALSE(
        Decimal::Parse("1")->Divide(Decimal::Zero()));
}
