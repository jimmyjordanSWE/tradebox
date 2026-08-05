#include "tradebox/core/indicator.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <type_traits>

namespace tradebox::core {
namespace {

std::expected<Decimal, IndicatorGraphError> Average(
    const Decimal& total, std::uint32_t count,
    const std::string& indicator_id) {
    const auto divisor = Decimal::Parse(std::to_string(count));
    if (!divisor)
        return std::unexpected(
            IndicatorGraphError{indicator_id, divisor.error().message});
    const auto average = total.Divide(*divisor, 9);
    if (!average)
        return std::unexpected(
            IndicatorGraphError{indicator_id, average.error().message});
    return *average;
}

const Decimal& BarValue(const MarketBar& bar, BarSeriesField field) {
    switch (field) {
        case BarSeriesField::Open: return bar.open;
        case BarSeriesField::High: return bar.high;
        case BarSeriesField::Low: return bar.low;
        case BarSeriesField::Close: return bar.close;
        case BarSeriesField::Volume: return bar.volume;
    }
    return bar.close;
}

std::vector<IndicatorPoint> BarInput(
    std::span<const MarketBar> bars, BarSeriesField field) {
    std::vector<IndicatorPoint> result;
    result.reserve(bars.size());
    for (const MarketBar& bar : bars)
        result.push_back({bar.start_ns, BarValue(bar, field)});
    return result;
}

std::expected<std::vector<IndicatorPoint>, IndicatorGraphError>
SimpleMovingAverage(const std::string& id,
                    std::span<const IndicatorPoint> input,
                    std::uint32_t period) {
    if (period == 0 || period > 10'000)
        return std::unexpected(IndicatorGraphError{
            id, "moving-average period must be between 1 and 10000"});
    std::vector<IndicatorPoint> result;
    if (input.size() < period) return result;
    result.reserve(input.size() - period + 1);
    Decimal rolling = Decimal::Zero();
    for (std::size_t index = 0; index < period; ++index)
        rolling += input[index].value;
    auto value = Average(rolling, period, id);
    if (!value) return std::unexpected(value.error());
    result.push_back({input[period - 1].start_ns, *value});
    for (std::size_t index = period; index < input.size(); ++index) {
        rolling -= input[index - period].value;
        rolling += input[index].value;
        value = Average(rolling, period, id);
        if (!value) return std::unexpected(value.error());
        result.push_back({input[index].start_ns, *value});
    }
    return result;
}

std::expected<std::vector<IndicatorPoint>, IndicatorGraphError>
ExponentialMovingAverage(const std::string& id,
                         std::span<const IndicatorPoint> input,
                         std::uint32_t period) {
    auto result = SimpleMovingAverage(id, input, period);
    if (!result || result->empty()) return result;
    const auto two = Decimal::Parse("2");
    const auto divisor = Decimal::Parse(std::to_string(period + 1));
    if (!two || !divisor)
        return std::unexpected(IndicatorGraphError{
            id, "could not construct EMA multiplier"});
    const auto multiplier = two->Divide(*divisor, 12);
    if (!multiplier)
        return std::unexpected(
            IndicatorGraphError{id, multiplier.error().message});
    result->resize(1);
    Decimal previous = result->front().value;
    for (std::size_t index = period; index < input.size(); ++index) {
        previous =
            (input[index].value - previous) * *multiplier + previous;
        result->push_back({input[index].start_ns, previous});
    }
    return result;
}

}  // namespace

std::expected<std::vector<IndicatorSeries>, IndicatorGraphError>
EvaluateIndicators(std::span<const IndicatorDefinition> definitions,
                   std::span<const MarketBar> bars) {
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (definitions[index].id.empty())
            return std::unexpected(
                IndicatorGraphError{{}, "indicator has no stable ID"});
        if (!indexes.emplace(definitions[index].id, index).second)
            return std::unexpected(IndicatorGraphError{
                definitions[index].id, "duplicate indicator identity"});
    }

    std::vector<IndicatorSeries> results(definitions.size());
    std::vector<unsigned char> states(definitions.size(), 0);
    std::function<std::expected<void, IndicatorGraphError>(std::size_t)>
        evaluate;
    evaluate = [&](std::size_t index)
        -> std::expected<void, IndicatorGraphError> {
        if (states[index] == 2) return {};
        const IndicatorDefinition& definition = definitions[index];
        if (states[index] == 1)
            return std::unexpected(IndicatorGraphError{
                definition.id, "indicator dependency cycle detected"});
        states[index] = 1;

        const IndicatorInput& source = std::visit(
            [](const auto& calculation) -> const IndicatorInput& {
                return calculation.input;
            },
            definition.calculation);
        std::vector<IndicatorPoint> input;
        if (const auto* bar =
                std::get_if<BarSeriesInput>(&source)) {
            input = BarInput(bars, bar->field);
        } else {
            const auto& reference =
                std::get<IndicatorOutputInput>(source);
            if (reference.output_id != "value")
                return std::unexpected(IndicatorGraphError{
                    definition.id,
                    "referenced indicator output does not exist"});
            const auto found = indexes.find(reference.indicator_id);
            if (found == indexes.end())
                return std::unexpected(IndicatorGraphError{
                    definition.id, "referenced indicator does not exist"});
            auto dependency = evaluate(found->second);
            if (!dependency) return std::unexpected(dependency.error());
            input = results[found->second].points;
        }

        auto points = std::visit(
            [&](const auto& calculation) {
                using T = std::decay_t<decltype(calculation)>;
                if constexpr (std::is_same_v<
                                  T, SimpleMovingAverageCalculation>)
                    return SimpleMovingAverage(
                        definition.id, input, calculation.period);
                else
                    return ExponentialMovingAverage(
                        definition.id, input, calculation.period);
            },
            definition.calculation);
        if (!points) return std::unexpected(points.error());
        results[index] = {
            .indicator_id = definition.id,
            .points = std::move(*points),
        };
        states[index] = 2;
        return {};
    };

    for (std::size_t index = 0; index < definitions.size(); ++index) {
        auto evaluated = evaluate(index);
        if (!evaluated) return std::unexpected(evaluated.error());
    }
    return results;
}

}  // namespace tradebox::core
