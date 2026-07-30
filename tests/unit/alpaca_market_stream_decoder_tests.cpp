#include "tradebox/broker/alpaca_market_stream_decoder.h"

#include <gtest/gtest.h>

#include <variant>

namespace {

using namespace tradebox;

TEST(AlpacaMarketStreamDecoder,
     DecodesControlsAndExactTypedEventsFromOneRawFrame) {
    constexpr std::string_view frame = R"([
      {"T":"success","msg":"authenticated"},
      {"T":"subscription","trades":["QQQ"],"quotes":["QQQ"]},
      {"T":"q","S":"QQQ","bx":"V","bp":500.01,"bs":10,
       "ax":"V","ap":500.02,"as":20,"c":["R"],"z":"C",
       "t":"2026-07-28T13:30:00.123456789Z"},
      {"T":"t","S":"QQQ","i":42,"x":"V","p":500.015,
       "s":100,"c":["@"],"z":"C",
       "t":"2026-07-28T13:30:00.123456790Z"}
    ])";

    const auto decoded =
        broker::alpaca::DecodeMarketFrame(
            frame, 1234,
            [](std::string_view) {
                return std::string("isin:US46090E1038");
            });

    ASSERT_EQ(decoded.controls.size(), 2U);
    EXPECT_EQ(
        decoded.controls[0].type,
        broker::alpaca::StreamControlType::Authenticated);
    EXPECT_EQ(decoded.controls[1].trade_symbols,
              std::vector<std::string>{"QQQ"});
    ASSERT_EQ(decoded.items.size(), 2U);

    const auto& quote = std::get<core::QuoteReceived>(
        *decoded.items[0].market_event);
    EXPECT_EQ(quote.quote.instrument_id,
              "isin:US46090E1038");
    EXPECT_EQ(quote.quote.bid_price.ToString(), "500.01");
    EXPECT_EQ(quote.quote.event_time_ns % 1'000'000,
              456789);

    const auto& trade = std::get<core::TradeReceived>(
        *decoded.items[1].market_event);
    EXPECT_EQ(trade.trade.trade_id, "42");
    EXPECT_EQ(trade.trade.price.ToString(), "500.015");
    EXPECT_EQ(decoded.items[1].source_event_id,
              "QQQ:" +
                  std::to_string(trade.trade.event_time_ns) +
                  ":42");
}

TEST(AlpacaMarketStreamDecoder,
     NormalizesTimestampOffsetsWithoutLosingNanoseconds) {
    constexpr std::string_view frame = R"([
      {"T":"t","S":"QQQ","i":"utc","x":"V","p":"500","s":"1",
       "c":[],"z":"C","t":"2026-07-28T13:30:00.123456789Z"},
      {"T":"t","S":"QQQ","i":"offset","x":"V","p":"500","s":"1",
       "c":[],"z":"C","t":"2026-07-28T16:00:00.123456789+02:30"}
    ])";

    const auto decoded =
        broker::alpaca::DecodeMarketFrame(
            frame, 1, [](std::string_view symbol) {
                return "asset:" + std::string(symbol);
            });

    ASSERT_EQ(decoded.items.size(), 2U);
    EXPECT_EQ(decoded.items[0].event_time_ns,
              decoded.items[1].event_time_ns);
    EXPECT_EQ(decoded.items[0].event_time_ns,
              1'785'245'400'123'456'789);
}

TEST(AlpacaMarketStreamDecoder,
     DecodesTradingHaltsAndResumeStates) {
    constexpr std::string_view frame = R"([
      {"T":"s","S":"AAPL","sc":"H","sm":"Trading Halt",
       "rc":"T12","rm":"Information requested by NASDAQ",
       "z":"C","t":"2026-07-28T13:30:00Z"},
      {"T":"s","S":"AAPL","sc":"T","sm":"Trading Resumption",
       "rc":"","rm":"","z":"C",
       "t":"2026-07-28T13:31:00Z"}
    ])";

    const auto decoded =
        broker::alpaca::DecodeMarketFrame(
            frame, 1234,
            [](std::string_view) {
                return std::string("asset-aapl");
            });

    ASSERT_EQ(decoded.items.size(), 2U);
    const auto& halt =
        std::get<core::TradingStatusReceived>(
            *decoded.items[0].market_event);
    EXPECT_EQ(halt.status.state,
              core::SecurityTradingState::Halted);
    EXPECT_TRUE(halt.status.BlocksNewOrders());
    EXPECT_EQ(halt.status.reason_code, "T12");
    const auto& resume =
        std::get<core::TradingStatusReceived>(
            *decoded.items[1].market_event);
    EXPECT_EQ(resume.status.state,
              core::SecurityTradingState::Trading);
    EXPECT_FALSE(resume.status.BlocksNewOrders());
}

TEST(AlpacaMarketStreamDecoder,
     TreatsNullOptionalFeedMetadataAsEmpty) {
    constexpr std::string_view frame = R"([
      {"T":"t","S":"QQQ","i":"trade-1","x":null,
       "p":"500.01","s":"10","c":null,"z":null,
       "t":"2026-07-28T13:30:00Z"}
    ])";

    const auto decoded =
        broker::alpaca::DecodeMarketFrame(
            frame, 1234,
            [](std::string_view symbol) {
                return std::string(symbol);
            });

    ASSERT_EQ(decoded.items.size(), 1U);
    const auto& trade = std::get<core::TradeReceived>(
        *decoded.items[0].market_event);
    EXPECT_TRUE(trade.trade.exchange.empty());
    EXPECT_TRUE(trade.trade.conditions.empty());
    EXPECT_TRUE(trade.trade.tape.empty());
}

TEST(AlpacaMarketStreamDecoder,
     DecodesProviderMinuteBarsAndLateRevisionsExactly) {
    constexpr std::string_view frame = R"([
      {"T":"b","S":"QQQ","o":500.01,"h":500.20,
       "l":499.90,"c":500.10,"v":12345,"vw":500.051,
       "n":321,"t":"2026-07-28T13:30:00Z"},
      {"T":"u","S":"QQQ","o":500.01,"h":500.25,
       "l":499.90,"c":500.15,"v":12400,"vw":500.052,
       "n":322,"t":"2026-07-28T13:30:00Z"}
    ])";

    const auto decoded =
        broker::alpaca::DecodeMarketFrame(
            frame, 1234,
            [](std::string_view) {
                return std::string("instrument-qqq");
            });

    ASSERT_EQ(decoded.items.size(), 2U);
    ASSERT_TRUE(decoded.items[0].bar);
    ASSERT_TRUE(decoded.items[1].bar);
    EXPECT_EQ(decoded.items[0].bar->kind, "b");
    EXPECT_EQ(decoded.items[0].bar->close.ToString(), "500.1");
    EXPECT_EQ(decoded.items[0].bar->volume.ToString(), "12345");
    EXPECT_EQ(decoded.items[0].bar->trade_count, 321U);
    EXPECT_EQ(
        decoded.items[1].bar->within_bar_vwap->ToString(),
        "500.052");
}

TEST(AlpacaMarketStreamDecoder, RejectsNonArrayPackets) {
    EXPECT_THROW(
        broker::alpaca::DecodeMarketFrame(
            R"({"T":"t"})", 1234,
            [](std::string_view symbol) {
                return std::string(symbol);
            }),
        std::runtime_error);
}

}  // namespace
