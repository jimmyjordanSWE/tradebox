#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/persistence/database_order_command_journal.h"
#include "tradebox/platform/credentials.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
namespace core = tradebox::core;
namespace application = tradebox::application;
namespace persistence = tradebox::persistence;

constexpr std::string_view kOptInValue =
    "I_UNDERSTAND_THIS_CREATES_PAPER_ORDERS";

class ContractSkipped final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string Environment(const char* name) {
    const DWORD required =
        GetEnvironmentVariableA(name, nullptr, 0);
    if (required == 0) return {};
    std::string result(required, '\0');
    const DWORD written = GetEnvironmentVariableA(
        name, result.data(), required);
    if (written == 0 || written >= required) return {};
    result.resize(written);
    return result;
}

class TemporaryDatabase final {
public:
    TemporaryDatabase() {
        const auto nonce =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        directory_ =
            std::filesystem::temp_directory_path() /
            ("tradebox-paper-contract-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(nonce));
        path_ = directory_ / "contract.db";
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    const std::filesystem::path& Path() const {
        return path_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

class LostCompletionJournal final
    : public core::IOrderCommandJournal {
public:
    explicit LostCompletionJournal(Database& database)
        : durable_(database) {}

    void LoseNextCompletion() {
        lose_next_completion_ = true;
        block_recovery_ = true;
    }

    std::expected<core::ReservationResult, std::string>
    Reserve(const core::OrderCommandRecord& record,
            const core::NativeOrderCommand& command) override {
        return durable_.Reserve(record, command);
    }

    std::expected<void, std::string> Complete(
        const core::OrderCommandResult& result) override {
        if (lose_next_completion_.exchange(false))
            return std::unexpected(
                "contract-injected response persistence loss");
        return durable_.Complete(result);
    }

    std::expected<void, std::string> MarkDispatchStarted(
        const std::string& request_id) override {
        return durable_.MarkDispatchStarted(request_id);
    }

    std::expected<
        std::vector<core::RecoverableOrderCommand>,
        std::string>
    Recoverable() override {
        return durable_.Recoverable();
    }

    std::expected<void, std::string> ResolveRecovery(
        const core::OrderCommandResult& result) override {
        if (block_recovery_)
            return std::unexpected(
                "contract holds recovery until restart");
        return durable_.ResolveRecovery(result);
    }

    std::expected<core::OrderCommandLookup, std::string>
    Lookup(const std::string& request_id) override {
        return durable_.Lookup(request_id);
    }

private:
    persistence::DatabaseOrderCommandJournal durable_;
    std::atomic<bool> lose_next_completion_ = false;
    std::atomic<bool> block_recovery_ = false;
};

template <typename Predicate>
void WaitFor(std::string_view description,
             std::chrono::seconds timeout,
             Predicate predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error(
        "timed out waiting for " + std::string(description));
}

const core::OrderState* FindOrder(
    const core::CoreSnapshot& snapshot,
    std::string_view order_id) {
    const auto found = std::ranges::find_if(
        snapshot.orders,
        [order_id](const core::OrderState& order) {
            return order.id == order_id;
        });
    return found == snapshot.orders.end() ? nullptr
                                          : &*found;
}

const core::OrderState* FindClientOrder(
    const core::CoreSnapshot& snapshot,
    std::string_view client_order_id) {
    const auto found = std::ranges::find_if(
        snapshot.orders,
        [client_order_id](const core::OrderState& order) {
            return order.client_order_id == client_order_id;
        });
    return found == snapshot.orders.end() ? nullptr
                                          : &*found;
}

bool IsCancelable(const core::OrderState& order) {
    return order.status == "new" ||
           order.status == "partially_filled" ||
           order.status == "held";
}

bool IsCanceled(const core::OrderState& order) {
    return order.status == "canceled" ||
           order.status == "cancelled";
}

bool IsTerminal(const core::OrderState& order) {
    return IsCanceled(order) ||
           order.status == "filled" ||
           order.status == "expired" ||
           order.status == "rejected" ||
           order.status == "replaced" ||
           order.status == "done_for_day" ||
           order.status == "stopped";
}

std::string UniqueId(std::string_view suffix) {
    const auto nonce =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    return "contract-" + std::to_string(GetCurrentProcessId()) +
           "-" + std::to_string(nonce) + "-" +
           std::string(suffix);
}

application::ConnectionRequest Connection(
    const std::string& key, const std::string& secret,
    const std::string& symbol) {
    return {
        .environment = core::AccountEnvironment::Paper,
        .api_key = key,
        .api_secret = secret,
        .market_symbols = {symbol},
        .market_data_feed = core::MarketDataFeed::Iex,
    };
}

core::OrderCommandContext Context(
    const core::CoreSnapshot& snapshot,
    std::string request_id) {
    if (!snapshot.account)
        throw std::runtime_error(
            "account identity is unavailable");
    return {
        .request_id = std::move(request_id),
        .source = "alpaca-paper-contract",
        .account_id = snapshot.account->id,
        .environment = core::AccountEnvironment::Paper,
        .generation = snapshot.generation,
    };
}

core::NativeOrderRequest RestingOrder(
    const std::string& symbol,
    const core::Decimal& limit,
    std::string client_order_id) {
    return {
        .asset_class = core::AssetClass::Equity,
        .symbol = symbol,
        .qty = *core::Decimal::Parse("1"),
        .side = core::OrderSide::Buy,
        .type = core::OrderType::Limit,
        .time_in_force = core::TimeInForce::Day,
        .order_class = core::OrderClass::Simple,
        .limit_price = limit,
        .extended_hours = true,
        .client_order_id = std::move(client_order_id),
    };
}

void ConnectAndWait(
    application::TradingApplication& app,
    const std::string& key, const std::string& secret,
    const std::string& symbol,
    std::uint64_t after_generation = 0) {
    const auto receipt =
        app.Connect(Connection(key, secret, symbol));
    if (!receipt || !receipt->accepted)
        throw std::runtime_error(
            "Paper connection command was rejected");
    WaitFor("authoritative Paper reconciliation", 45s, [&] {
        const auto snapshot = app.Snapshot();
        return snapshot.environment ==
                   core::AccountEnvironment::Paper &&
               snapshot.generation.value > after_generation &&
               snapshot.safety_status ==
                   core::SafetyStatus::Live &&
               snapshot.authenticated &&
               snapshot.trade_updates_acknowledged &&
               snapshot.initial_snapshot_loaded &&
               snapshot.reconciled &&
               snapshot.trading_permitted &&
               snapshot.account.has_value();
    });
}

core::OrderCommandResult Submit(
    application::TradingApplication& app,
    core::NativeOrderCommand command) {
    auto future = app.SubmitOrder(std::move(command));
    if (future.wait_for(30s) != std::future_status::ready)
        throw std::runtime_error(
            "order command did not complete within 30 seconds");
    return future.get();
}

void RequireAccepted(const core::OrderCommandResult& result,
                     std::string_view operation) {
    if (!result.AcceptedByBroker())
        throw std::runtime_error(
            std::string(operation) + " failed: " +
            result.message + " (HTTP " +
            std::to_string(result.http_status) + ")");
}

void CancelBestEffort(
    application::TradingApplication& app,
    const std::string& order_id) {
    if (order_id.empty()) return;
    const auto snapshot = app.Snapshot();
    if (!snapshot.account) return;
    const auto* order = FindOrder(snapshot, order_id);
    if (!order || IsTerminal(*order)) return;
    static_cast<void>(Submit(
        app, core::CancelOrderCommand{
                 .context =
                     Context(snapshot, UniqueId("cleanup")),
                 .order_id = order_id,
             }));
}

void CancelExistingContractOrders(
    application::TradingApplication& app) {
    const auto snapshot = app.Snapshot();
    for (const core::OrderState& order : snapshot.orders) {
        if (!order.client_order_id.starts_with(
                "tb-contract-") ||
            IsTerminal(order))
            continue;
        const auto result = Submit(
            app, core::CancelOrderCommand{
                     .context = Context(
                         app.Snapshot(),
                         UniqueId("prior-cleanup")),
                     .order_id = order.id,
                 });
        RequireAccepted(result, "prior contract cleanup");
        WaitFor("prior contract order cancellation", 30s,
                [&] {
                    const auto current = app.Snapshot();
                    const auto* found =
                        FindOrder(current, order.id);
                    return found && IsCanceled(*found);
                });
    }
}

void WaitForReplaceableOrder(
    application::TradingApplication& app,
    const std::string& order_id) {
    const auto deadline =
        std::chrono::steady_clock::now() + 20s;
    std::string last_status = "missing";
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = app.Snapshot();
        const auto* order =
            FindOrder(snapshot, order_id);
        if (order) {
            last_status = order->status;
            if (IsCancelable(*order)) return;
            if (IsTerminal(*order))
                throw std::runtime_error(
                    "resting contract order became terminal "
                    "before replacement: " +
                    order->status);
        }
        std::this_thread::sleep_for(100ms);
    }
    if (last_status == "accepted")
        throw ContractSkipped(
            "Paper order remained accepted and cannot be "
            "replaced outside its active trading session");
    throw std::runtime_error(
        "order did not become replaceable; last status was " +
        last_status);
}

void VerifyRateHeaders(
    const application::TradingApplication& app) {
    WaitFor("Alpaca trading rate-limit headers", 10s, [&] {
        const auto health = app.RestHealth();
        return health.trading.limit >= 0 &&
               health.trading.remaining >= 0 &&
               health.trading.reset_at_ms > 0;
    });
}

void RunContract(const std::string& key,
                 const std::string& secret,
                 const std::string& symbol,
                 const core::Decimal& initial_limit,
                 const core::Decimal& replacement_limit) {
    TemporaryDatabase temporary;
    Database database;
    std::string database_error;
    if (!database.OpenAt(temporary.Path(), database_error))
        throw std::runtime_error(
            "temporary database open failed: " +
            database_error);

    const std::string normal_request = UniqueId("place");
    const std::string normal_client_id =
        "tb-" + normal_request;
    std::string ambiguous_request;
    std::string ambiguous_client_id;

    {
        LostCompletionJournal journal(database);
        application::TradingApplication app(database, journal);
        ConnectAndWait(app, key, secret, symbol);
        CancelExistingContractOrders(app);

        const auto connected = app.Snapshot();
        const std::uint64_t first_generation =
            connected.generation.value;
        const auto disconnected = app.Disconnect();
        if (!disconnected || !disconnected->accepted)
            throw std::runtime_error(
                "disconnect command was rejected");
        WaitFor("explicit disconnected state", 10s, [&] {
            return app.Snapshot().safety_status ==
                   core::SafetyStatus::Disconnected;
        });
        ConnectAndWait(app, key, secret, symbol,
                       first_generation);

        std::string active_order_id;
        try {
            auto before = app.Snapshot();
            const auto placed = Submit(
                app, core::PlaceOrderCommand{
                         .context =
                             Context(before, normal_request),
                         .order = RestingOrder(
                             symbol, initial_limit,
                             normal_client_id),
                     });
            RequireAccepted(placed, "place");
            active_order_id = placed.broker_order_id;

            const auto before_rest_ms =
                before.orders_received_at_ms;
            const auto before_stream_ms =
                before.last_trade_update_at_ms;
            WaitFor("placed order in REST snapshot", 30s, [&] {
                const auto snapshot = app.Snapshot();
                return snapshot.orders_received_at_ms >
                           before_rest_ms &&
                       FindOrder(snapshot,
                                 active_order_id) != nullptr;
            });
            WaitFor("placed order on trade_updates", 30s, [&] {
                const auto snapshot = app.Snapshot();
                return snapshot.last_trade_update_at_ms >
                       before_stream_ms;
            });
            WaitForReplaceableOrder(app, active_order_id);

            before = app.Snapshot();
            const auto before_replace_stream_ms =
                before.last_trade_update_at_ms;
            const std::string replace_request =
                UniqueId("replace");
            const auto replaced = Submit(
                app, core::ReplaceOrderCommand{
                         .context =
                             Context(before, replace_request),
                         .order_id = active_order_id,
                         .replacement = {
                             .limit_price =
                                 replacement_limit,
                         },
                     });
            RequireAccepted(replaced, "replace");
            const std::string original_order_id =
                active_order_id;
            active_order_id = replaced.broker_order_id;
            WaitFor("replacement in authoritative state", 30s,
                    [&] {
                        const auto snapshot = app.Snapshot();
                        const auto* original =
                            FindOrder(snapshot,
                                      original_order_id);
                        return FindOrder(snapshot,
                                         active_order_id) !=
                                   nullptr &&
                               original &&
                               !original->replaced_by.empty();
                    });
            WaitFor("replacement trade_update", 30s, [&] {
                const auto snapshot = app.Snapshot();
                return snapshot.last_trade_update_at_ms >
                       before_replace_stream_ms;
            });
            before = app.Snapshot();
            const auto before_cancel_stream_ms =
                before.last_trade_update_at_ms;
            const auto canceled = Submit(
                app, core::CancelOrderCommand{
                         .context = Context(
                             before, UniqueId("cancel")),
                         .order_id = active_order_id,
                     });
            RequireAccepted(canceled, "cancel");
            WaitFor("authoritative canceled state", 30s, [&] {
                const auto snapshot = app.Snapshot();
                const auto* order =
                    FindOrder(snapshot, active_order_id);
                return order && IsCanceled(*order) &&
                       snapshot.last_trade_update_at_ms >
                           before_cancel_stream_ms;
            });
            active_order_id.clear();

            VerifyRateHeaders(app);

            ambiguous_request = UniqueId("ambiguous-place");
            ambiguous_client_id =
                "tb-" + ambiguous_request;
            journal.LoseNextCompletion();
            const auto ambiguous = Submit(
                app, core::PlaceOrderCommand{
                         .context = Context(
                             app.Snapshot(),
                             ambiguous_request),
                         .order = RestingOrder(
                             symbol, initial_limit,
                             ambiguous_client_id),
                     });
            if (ambiguous.outcome !=
                    core::OrderCommandOutcome::Indeterminate ||
                ambiguous.recovery_state !=
                    core::CommandRecoveryState::Pending ||
                ambiguous.broker_order_id.empty())
                throw std::runtime_error(
                    "injected response loss did not create "
                    "a recoverable indeterminate command");
            active_order_id = ambiguous.broker_order_id;
            static_cast<void>(app.Disconnect());
        } catch (...) {
            CancelBestEffort(app, active_order_id);
            throw;
        }
    }

    {
        application::TradingApplication app(database);
        ConnectAndWait(app, key, secret, symbol);
        std::string recovered_order_id;
        try {
            WaitFor("ambiguous order in authoritative snapshot",
                    30s, [&] {
                        const auto snapshot = app.Snapshot();
                        const auto* order =
                            FindClientOrder(
                                snapshot,
                                ambiguous_client_id);
                        if (!order) return false;
                        recovered_order_id = order->id;
                        return true;
                    });
            WaitFor("durable ambiguous-command recovery", 30s,
                    [&] {
                        const auto lookup =
                            app.OrderCommandStatus(
                                ambiguous_request);
                        return lookup &&
                               lookup->terminal_result &&
                               lookup->recovery_state ==
                                   core::CommandRecoveryState::
                                       Resolved &&
                               lookup->terminal_result
                                       ->broker_order_id ==
                                   recovered_order_id;
                    });

            const auto snapshot = app.Snapshot();
            const auto matching = std::ranges::count_if(
                snapshot.orders,
                [&](const core::OrderState& order) {
                    return order.client_order_id ==
                           ambiguous_client_id;
                });
            if (matching != 1)
                throw std::runtime_error(
                    "ambiguous recovery found " +
                    std::to_string(matching) +
                    " broker orders for one client_order_id");

            const auto canceled = Submit(
                app, core::CancelOrderCommand{
                         .context = Context(
                             app.Snapshot(),
                             UniqueId(
                                 "ambiguous-cancel")),
                         .order_id = recovered_order_id,
                     });
            RequireAccepted(canceled,
                            "recovered-order cancel");
            WaitFor("recovered order cancellation", 30s,
                    [&] {
                        const auto current = app.Snapshot();
                        const auto* order =
                            FindOrder(current,
                                      recovered_order_id);
                        return order && IsCanceled(*order);
                    });
            recovered_order_id.clear();
            VerifyRateHeaders(app);
            static_cast<void>(app.Disconnect());
        } catch (...) {
            CancelBestEffort(app, recovered_order_id);
            throw;
        }
    }
}

void CleanupContractOrders(
    const std::string& key, const std::string& secret,
    const std::string& symbol) {
    TemporaryDatabase temporary;
    Database database;
    std::string database_error;
    if (!database.OpenAt(temporary.Path(), database_error))
        throw std::runtime_error(
            "temporary database open failed: " +
            database_error);
    application::TradingApplication app(database);
    ConnectAndWait(app, key, secret, symbol);
    CancelExistingContractOrders(app);
    const auto snapshot = app.Snapshot();
    const bool open_contract_order =
        std::ranges::any_of(
            snapshot.orders,
            [](const core::OrderState& order) {
                return order.client_order_id.starts_with(
                           "tb-contract-") &&
                       !IsTerminal(order);
            });
    if (open_contract_order)
        throw std::runtime_error(
            "an open contract order remains after cleanup");
    static_cast<void>(app.Disconnect());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (Environment(
                "TRADEBOX_ALPACA_PAPER_CONTRACT") !=
            kOptInValue)
            throw std::runtime_error(
                "refusing to run: set "
                "TRADEBOX_ALPACA_PAPER_CONTRACT="
                "I_UNDERSTAND_THIS_CREATES_PAPER_ORDERS");

        std::string key =
            Environment("APCA_API_KEY_ID");
        std::string secret =
            Environment("APCA_API_SECRET_KEY");
        if (key.empty() && secret.empty()) {
            AlpacaCredentials saved;
            std::string credential_error;
            if (!CredentialStore::Load(
                    true, saved, credential_error))
                throw std::runtime_error(
                    "Paper credentials are unavailable: " +
                    credential_error);
            if (!saved.paper)
                throw std::runtime_error(
                    "credential store returned non-Paper "
                    "credentials");
            key = std::move(saved.key);
            secret = std::move(saved.secret);
        } else if (key.empty() || secret.empty()) {
            throw std::runtime_error(
                "APCA_API_KEY_ID and APCA_API_SECRET_KEY "
                "must either both be set or both be absent");
        }

        const std::string configured_base =
            Environment("APCA_API_BASE_URL");
        if (!configured_base.empty() &&
            configured_base !=
                "https://paper-api.alpaca.markets")
            throw std::runtime_error(
                "APCA_API_BASE_URL must be unset or exactly "
                "https://paper-api.alpaca.markets");

        std::string symbol =
            Environment("TRADEBOX_ALPACA_CONTRACT_SYMBOL");
        if (symbol.empty()) symbol = "SPY";
        std::string initial_text =
            Environment(
                "TRADEBOX_ALPACA_CONTRACT_LIMIT_PRICE");
        if (initial_text.empty()) initial_text = "1.00";
        std::string replacement_text =
            Environment(
                "TRADEBOX_ALPACA_CONTRACT_REPLACEMENT_PRICE");
        if (replacement_text.empty())
            replacement_text = "1.01";
        const auto initial =
            core::Decimal::Parse(initial_text);
        const auto replacement =
            core::Decimal::Parse(replacement_text);
        if (!initial || !replacement ||
            *initial <= core::Decimal{} ||
            *replacement <= core::Decimal{})
            throw std::runtime_error(
                "contract limit prices must be positive "
                "exact decimals");

        const bool cleanup_only =
            argc == 2 &&
            std::string_view(argv[1]) == "--cleanup-only";
        if (argc > 2 || (argc == 2 && !cleanup_only))
            throw std::runtime_error(
                "only the optional --cleanup-only argument "
                "is supported");
        if (cleanup_only) {
            CleanupContractOrders(key, secret, symbol);
            std::cout
                << "ALPACA PAPER CONTRACT CLEANUP PASSED"
                << '\n';
            return 0;
        }

        RunContract(key, secret, symbol, *initial,
                    *replacement);
        std::cout
            << "ALPACA PAPER CONTRACT PASSED | "
            << "reconcile, disconnect/reconnect, "
            << "place, websocket+REST observation, replace, "
            << "cancel, response-loss recovery, no duplicate, "
            << "rate headers"
            << '\n';
        return 0;
    } catch (const ContractSkipped& skipped) {
        std::cout << "ALPACA PAPER CONTRACT SKIPPED: "
                  << skipped.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "ALPACA PAPER CONTRACT FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
