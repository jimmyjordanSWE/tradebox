#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/platform/credentials.h"
#include "tradebox/ui/model.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

namespace {

constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;
int g_title_bar_height = 30;
constexpr int kResizeBorder = 6;
constexpr int kInteractiveTitleBarWidth = 190;
constexpr int kWindowControlsWidth = 126;
constexpr int kTitleStatusWidth = 460;

enum class ConnectionState {
    Disconnected,
    Connecting,
    AccountAuthenticated,
    Streaming,
    Error
};

struct LaunchOptions {
    std::filesystem::path capture_path;
    bool capture_and_exit = false;
    bool connect_paper = false;
    int run_for_ms = 0;
};

struct Chart {
    std::string symbol;
    std::string timeframe = "1Day";
    std::vector<Bar> bars;
    int visible_bars = 120;
    double last_trade_price = 0;
    std::int64_t last_trade_timestamp_ms = 0;
    bool window_open = false;
};

struct App {
    explicit App(Database& db)
        : database(db),
          application(events, database) {}

    Database& database;
    UiEventQueue events;
    tradebox::application::TradingApplication application;
    std::vector<std::string> watchlist;
    std::vector<std::string> requested_market_symbols;
    std::unordered_map<std::string, Chart> charts;
    struct MarketProjection {
        tradebox::core::MarketDataSnapshot snapshot;
        std::uint64_t cursor = 0;
    };
    std::unordered_map<std::string, MarketProjection> market_views;
    std::uint64_t market_change_cursor = 0;
    // Read-only projections copied from the headless core for rendering.
    tradebox::core::CoreSnapshot core_view;
    tradebox::core::AccountState account;
    bool has_account = false;
    std::string account_alias;
    std::array<char, 96> account_alias_edit{};
    std::vector<tradebox::core::PositionState> positions;
    std::vector<tradebox::core::OrderState> orders;
    std::int64_t positions_received_at_ms = 0;
    std::int64_t orders_received_at_ms = 0;
    MarketClockSnapshot market_clock;
    bool has_market_clock = false;
    bool active_paper = true;
    bool credentials_open = false;
    bool show_account = true;
    bool show_watchlist = true;
    bool show_event_log = true;
    bool show_orders = true;
    bool show_positions = true;
    bool show_time_sales = true;
    ConnectionState connection_state = ConnectionState::Disconnected;
    std::int64_t market_latency_ms = -1;
    std::int64_t last_market_received_at_ms = 0;
    bool market_subscription_active = false;
    std::int64_t account_stream_latency_ms = -1;
    std::int64_t last_account_stream_event_at_ms = 0;
    std::int64_t account_event_latency_ms = -1;
    bool account_stream_failed = false;
    bool credential_paper = true;
    std::array<char, 160> key{};
    std::array<char, 160> secret{};
    std::array<char, 24> new_symbol{};
    std::vector<std::string> messages;
    struct OrderTicket {
        OrderEntryDraft draft;
        std::array<char, 96> name{'U','n','t','i','t','l','e','d',' ','o','r','d','e','r'};
        std::array<char, 24> symbol{'A','M','D'};
        std::array<char, 64> amount{'1'};
        std::array<char, 64> limit{};
        std::array<char, 64> stop{};
        std::string symbol_query;
        std::vector<tradebox::core::TradableAsset> symbol_matches;
        int symbol_cursor = 0;
        bool window_open = true;
        bool was_visible = false;
    };
    std::vector<OrderTicket> order_tickets;
    std::vector<tradebox::core::TradableAsset> asset_catalog;
    std::array<char, 96> asset_query{};
    int asset_cursor = 0;
    std::vector<tradebox::core::TradableAsset> asset_matches;
    std::string watchlist_asset_query;
    std::string selected_symbol = "AMD";
    std::array<char, 24> time_sales_symbol{'A','M','D'};
    std::string time_sales_query;
    std::vector<tradebox::core::TradableAsset> time_sales_matches;
    int time_sales_cursor = 0;
    std::size_t linked_order_ticket = 0;
    bool show_order_entry = true;
    bool command_in_flight = false;
    std::string command_request_id;
    std::string command_status;
    std::uint64_t next_request_id = 1;
    std::vector<std::jthread> command_workers;
    bool capture_requested = false;
    std::filesystem::path capture_path;
    bool exit_after_capture = false;
    int frames_before_capture = 0;
    bool vsync_requested = false;
    bool vsync_enabled = false;
    int swap_interval = 0;
    float display_refresh_hz = 0;
    int maximum_frame_rate = 0;
    bool software_frame_pacing = false;
    Uint64 next_frame_deadline_ns = 0;
    MarketDataStorageUsage market_storage;
    Uint64 market_storage_sampled_at = 0;
    DatabaseWriterTelemetry writer_telemetry;
    Uint64 writer_telemetry_sampled_at = 0;
};

void RequestChartData(App& app, Chart& chart);

void AddMessage(App& app, std::string message) {
    app.messages.push_back(std::move(message));
    if (app.messages.size() > 100)
        app.messages.erase(app.messages.begin(), app.messages.begin() + 20);
}

void ApplyPresentationSettings(App& app, SDL_Window* window, bool announce) {
    app.software_frame_pacing = app.maximum_frame_rate > 0;
    app.next_frame_deadline_ns = 0;
    app.vsync_enabled = false;
    if (app.vsync_requested) {
        app.vsync_enabled = SDL_GL_SetSwapInterval(-1);
        if (!app.vsync_enabled)
            app.vsync_enabled = SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
    int accepted_interval = 0;
    if (app.vsync_enabled &&
        SDL_GL_GetSwapInterval(&accepted_interval))
        app.swap_interval = accepted_interval;
    else
        app.swap_interval = 0;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    app.display_refresh_hz = mode ? mode->refresh_rate : 0;
    if (announce) {
        std::ostringstream message;
        if (app.vsync_enabled) {
            message << (app.swap_interval == -1
                            ? "VSync active (adaptive)"
                            : "VSync active (standard)");
        } else if (app.vsync_requested) {
            message << "VSync requested but unavailable";
        } else {
            message << "VSync off";
        }
        if (app.maximum_frame_rate > 0)
            message << "; capped at " << app.maximum_frame_rate << " FPS";
        else
            message << "; frame rate uncapped";
        if (app.display_refresh_hz > 0)
            message << " on " << std::fixed << std::setprecision(0)
                    << app.display_refresh_hz << " Hz display";
        AddMessage(app, message.str());
    }
}

void PaceSoftwareFrame(App& app) {
    if (!app.software_frame_pacing || app.maximum_frame_rate <= 0) return;
    const Uint64 period_ns = static_cast<Uint64>(
        1000000000.0 / static_cast<double>(app.maximum_frame_rate));
    const Uint64 now = SDL_GetTicksNS();
    if (app.next_frame_deadline_ns == 0 ||
        now > app.next_frame_deadline_ns + period_ns * 4)
        app.next_frame_deadline_ns = now + period_ns;
    else
        app.next_frame_deadline_ns += period_ns;
    const Uint64 before_wait = SDL_GetTicksNS();
    if (before_wait < app.next_frame_deadline_ns)
        SDL_DelayPrecise(app.next_frame_deadline_ns - before_wait);
}

std::string NormalizeSymbol(const char* input) {
    std::string symbol;
    for (const unsigned char character : std::string(input)) {
        if (std::isalnum(character) || character == '.' || character == '-')
            symbol.push_back(static_cast<char>(std::toupper(character)));
    }
    return symbol;
}

bool PlacementTouchesDisplay(const WindowPlacement& placement) {
    const SDL_Rect window_rect{placement.x, placement.y, placement.width,
                               placement.height};
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (!displays) return false;
    for (int index = 0; index < display_count; ++index) {
        SDL_Rect usable{};
        if (!SDL_GetDisplayUsableBounds(displays[index], &usable)) continue;
        SDL_Rect intersection{};
        if (SDL_GetRectIntersection(&window_rect, &usable, &intersection) &&
            intersection.w >= 100 && intersection.h >= 100) {
            SDL_free(displays);
            return true;
        }
    }
    SDL_free(displays);
    return false;
}

std::int64_t SystemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string FormatDelay(std::int64_t milliseconds);

std::filesystem::path DefaultScreenshotPath(const Database& database) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) %
        1000;
    std::ostringstream filename;
    filename << "tradebox-" << std::put_time(&local, "%Y%m%d-%H%M%S") << '-'
             << std::setw(3) << std::setfill('0') << milliseconds.count()
             << ".bmp";
    return database.DataDirectory() / "screenshots" / filename.str();
}

bool CaptureOpenGLFramebuffer(int width, int height,
                              const std::filesystem::path& path,
                              std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "Framebuffer has no drawable area";
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "Could not create screenshot directory: " +
                    filesystem_error.message();
            return false;
        }
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4);
    GLint previous_alignment = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previous_alignment);
    const GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        error = "OpenGL framebuffer read failed (" +
                std::to_string(static_cast<unsigned int>(gl_error)) + ')';
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> row(row_bytes);
    for (int top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
        std::uint8_t* top_row = pixels.data() + top * row_bytes;
        std::uint8_t* bottom_row = pixels.data() + bottom * row_bytes;
        std::memcpy(row.data(), top_row, row_bytes);
        std::memcpy(top_row, bottom_row, row_bytes);
        std::memcpy(bottom_row, row.data(), row_bytes);
    }

    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                              pixels.data(), static_cast<int>(row_bytes));
    if (!surface) {
        error = SDL_GetError();
        return false;
    }
    const bool save_result = SDL_SaveBMP(surface, path.string().c_str());
    if (!save_result) error = SDL_GetError();
    SDL_DestroySurface(surface);
    return save_result;
}

void UpsertDailyBar(Chart& chart, const Bar& bar) {
    const std::int64_t day = bar.timestamp_ms / kDayMs;
    auto same_day = std::find_if(chart.bars.begin(), chart.bars.end(),
                                 [day](const Bar& existing) {
                                     return existing.timestamp_ms / kDayMs == day;
                                 });
    if (same_day != chart.bars.end()) {
        *same_day = bar;
        return;
    }
    auto position = std::lower_bound(
        chart.bars.begin(), chart.bars.end(), bar.timestamp_ms,
        [](const Bar& existing, std::int64_t timestamp) {
            return existing.timestamp_ms < timestamp;
        });
    if (position != chart.bars.end() &&
        position->timestamp_ms == bar.timestamp_ms) {
        *position = bar;
    } else {
        chart.bars.insert(position, bar);
    }
}

void ApplyTrade(Chart& chart, const Bar& trade) {
    if (trade.close <= 0) return;
    chart.last_trade_price = trade.close;
    chart.last_trade_timestamp_ms = trade.timestamp_ms;
    const std::int64_t day = (trade.timestamp_ms / kDayMs) * kDayMs;
    if (chart.bars.empty() ||
        chart.bars.back().timestamp_ms / kDayMs != day / kDayMs) {
        chart.bars.push_back(
            {day, trade.close, trade.close, trade.close, trade.close, trade.volume});
        return;
    }
    Bar& current = chart.bars.back();
    current.high = std::max(current.high, trade.close);
    current.low = std::min(current.low, trade.close);
    current.close = trade.close;
    current.volume += trade.volume;
}

double PreviousSessionClose(const Chart& chart) {
    if (chart.last_trade_timestamp_ms <= 0) return 0;
    const std::int64_t trade_day = chart.last_trade_timestamp_ms / kDayMs;
    for (auto bar = chart.bars.rbegin(); bar != chart.bars.rend(); ++bar) {
        if (bar->timestamp_ms / kDayMs < trade_day) return bar->close;
    }
    return 0;
}

void RefreshCoreProjection(App& app) {
    auto snapshot =
        app.application.SnapshotAfter(app.core_view.revision);
    if (!snapshot) return;

    const bool first_account =
        !app.has_account && snapshot->account.has_value();
    app.core_view = std::move(*snapshot);
    app.has_account = app.core_view.account.has_value();
    app.account = app.core_view.account.value_or(
        tradebox::core::AccountState{});
    app.positions = app.core_view.positions;
    app.orders = app.core_view.orders;
    app.positions_received_at_ms =
        app.core_view.positions_received_at_ms;
    app.orders_received_at_ms = app.core_view.orders_received_at_ms;
    app.last_account_stream_event_at_ms =
        app.core_view.last_trade_update_at_ms;

    if (first_account) {
        if (app.connection_state == ConnectionState::Connecting)
            app.connection_state =
                ConnectionState::AccountAuthenticated;
        app.database.SaveLastConnectedPaper(app.active_paper);
        const std::string account_key =
            app.account.id.empty() ? app.account.account_number
                                   : app.account.id;
        app.account_alias = app.database.LoadAccountAlias(account_key);
        if (app.account_alias.empty())
            app.account_alias =
                app.active_paper ? "Paper account" : "Live account";
        std::snprintf(app.account_alias_edit.data(),
                      app.account_alias_edit.size(), "%s",
                      app.account_alias.c_str());
        AddMessage(app, "Account authenticated: " +
                            app.account.account_number);
    }
}

void InsertProjectedTrade(
    std::deque<tradebox::core::MarketTrade>& trades,
    tradebox::core::MarketTrade trade) {
    if (!trade.trade_id.empty()) {
        const auto duplicate = std::ranges::find_if(
            trades, [&trade](const auto& existing) {
                constexpr std::int64_t day_ns =
                    24LL * 60 * 60 * 1'000'000'000;
                return existing.trade_id == trade.trade_id &&
                       (existing.event_time_ns <= 0 ||
                        trade.event_time_ns <= 0 ||
                        existing.event_time_ns / day_ns ==
                            trade.event_time_ns / day_ns);
            });
        if (duplicate != trades.end()) {
            *duplicate = std::move(trade);
        } else if (trades.empty() ||
                   trades.front().event_time_ns <= trade.event_time_ns) {
            trades.push_front(std::move(trade));
        } else {
            const auto position = std::ranges::find_if(
                trades, [&trade](const auto& existing) {
                    return existing.event_time_ns < trade.event_time_ns;
                });
            trades.insert(position, std::move(trade));
        }
    } else {
        if (trades.empty() ||
            trades.front().event_time_ns <= trade.event_time_ns) {
            trades.push_front(std::move(trade));
        } else {
            const auto position = std::ranges::find_if(
                trades, [&trade](const auto& existing) {
                    return existing.event_time_ns < trade.event_time_ns;
                });
            trades.insert(position, std::move(trade));
        }
    }
    if (trades.size() > 2'000) trades.pop_back();
}

void RefreshMarketProjection(App& app, const std::string& symbol) {
    auto& projection = app.market_views[symbol];
    auto delta = app.application.MarketDataChanges(
        symbol, projection.cursor, 512);
    if (delta.gap_detected) {
        projection.snapshot = app.application.MarketData(symbol);
        projection.cursor = delta.next_sequence;
        return;
    }
    auto& snapshot = projection.snapshot;
    snapshot.symbol = symbol;
    snapshot.feed = delta.feed;
    snapshot.stream_status = delta.stream_status;
    snapshot.trades_subscribed = delta.trades_subscribed;
    snapshot.quotes_subscribed = delta.quotes_subscribed;
    snapshot.status_message = std::move(delta.status_message);
    snapshot.last_received_at_ms = delta.last_received_at_ms;
    for (auto& sequenced : delta.events) {
        std::visit(
            [&](auto& event) {
                using Event = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<
                                  Event,
                                  tradebox::core::QuoteReceived>) {
                    if (event.quote.received_at_ms > 0 &&
                        event.quote.event_time_ns > 0) {
                        app.market_latency_ms =
                            event.quote.received_at_ms -
                            event.quote.event_time_ns / 1'000'000;
                        app.last_market_received_at_ms =
                            event.quote.received_at_ms;
                    }
                    snapshot.latest_quote =
                        std::move(event.quote);
                } else if constexpr (std::is_same_v<
                                  Event,
                                  tradebox::core::TradeReceived>) {
                    if (event.trade.received_at_ms > 0 &&
                        event.trade.event_time_ns > 0) {
                        app.market_latency_ms =
                            event.trade.received_at_ms -
                            event.trade.event_time_ns / 1'000'000;
                        app.last_market_received_at_ms =
                            event.trade.received_at_ms;
                    }
                    if (auto chart = app.charts.find(symbol);
                        chart != app.charts.end()) {
                        ApplyTrade(
                            chart->second,
                            {event.trade.event_time_ns / 1'000'000,
                             0, 0, 0,
                             event.trade.price.ToDisplayDouble(),
                             event.trade.size.ToDisplayDouble()});
                    }
                    InsertProjectedTrade(snapshot.trades,
                                         std::move(event.trade));
                } else if constexpr (std::is_same_v<
                                         Event,
                                         tradebox::core::TradeCanceled>) {
                    std::erase_if(
                        snapshot.trades,
                        [&](const auto& trade) {
                            constexpr std::int64_t day_ns =
                                24LL * 60 * 60 * 1'000'000'000;
                            return trade.trade_id == event.trade_id &&
                                   (event.event_time_ns <= 0 ||
                                    trade.event_time_ns / day_ns ==
                                        event.event_time_ns / day_ns);
                        });
                } else if constexpr (std::is_same_v<
                                         Event,
                                         tradebox::core::TradeCorrected>) {
                    std::erase_if(
                        snapshot.trades,
                        [&](const auto& trade) {
                            return trade.trade_id ==
                                   event.original_trade_id;
                        });
                    InsertProjectedTrade(
                        snapshot.trades,
                        std::move(event.corrected_trade));
                }
            },
            *sequenced.event);
    }
    projection.cursor = delta.next_sequence;
    snapshot.revision = projection.cursor;
}

void RefreshVisibleMarketData(App& app) {
    auto changes = app.application.ChangedMarketInstruments(
        app.market_change_cursor);
    if (changes.instruments.empty() &&
        !changes.gap_detected) {
        app.market_change_cursor = changes.next_sequence;
        return;
    }
    if (changes.gap_detected) {
        for (const auto& symbol : app.requested_market_symbols)
            RefreshMarketProjection(app, symbol);
    } else {
        std::unordered_set<std::string> refreshed;
        refreshed.reserve(changes.instruments.size());
        for (const auto& change : changes.instruments) {
            if (std::ranges::binary_search(
                    app.requested_market_symbols,
                    change.symbol) &&
                refreshed.insert(change.symbol).second)
                RefreshMarketProjection(app, change.symbol);
        }
    }
    app.market_change_cursor = changes.next_sequence;
}

void RefreshRequiredMarketSymbols(App& app) {
    if (app.connection_state == ConnectionState::Disconnected ||
        app.connection_state == ConnectionState::Error)
        return;
    std::vector<std::string> required = app.watchlist;
    if (!app.selected_symbol.empty())
        required.push_back(app.selected_symbol);
    for (const auto& ticket : app.order_tickets)
        if (!ticket.draft.symbol.empty())
            required.push_back(ticket.draft.symbol);
    for (const auto& position : app.positions)
        if (!position.symbol.empty())
            required.push_back(position.symbol);
    for (const auto& order : app.orders)
        if (!order.symbol.empty())
            required.push_back(order.symbol);
    std::ranges::sort(required);
    required.erase(std::unique(required.begin(), required.end()),
                   required.end());
    if (required == app.requested_market_symbols) return;
    app.application.RefreshMarketSymbols(required);
    app.requested_market_symbols = std::move(required);
}

const tradebox::core::MarketDataSnapshot& MarketView(
    App& app, const std::string& symbol) {
    auto found = app.market_views.find(symbol);
    if (found == app.market_views.end()) {
        RefreshMarketProjection(app, symbol);
        found = app.market_views.find(symbol);
    }
    return found->second.snapshot;
}

void DrainEvents(App& app) {
    RefreshCoreProjection(app);
    for (UiEvent& event : app.events.Drain()) {
        if (event.type == UiEventType::Status) {
            app.database.QueueTimelineEvent(
                "tradebox.connection", "", "connection_status", "",
                SystemNowMs(),
                nlohmann::json({{"message", event.message}}).dump());
            if (event.message == "Market stream authenticated") {
                app.connection_state = ConnectionState::Streaming;
            } else if (event.message.starts_with(
                           "Market subscription active")) {
                app.market_subscription_active = true;
            } else if (event.message == "Market stream disconnected") {
                app.connection_state = ConnectionState::Disconnected;
                app.market_subscription_active = false;
            } else if (event.message.starts_with("Account login failed") ||
                       event.message.starts_with("Account JSON error") ||
                       event.message.starts_with(
                           "Market stream connection failed") ||
                       event.message.starts_with("WebSocket upgrade failed") ||
                       event.message.starts_with("Stream error")) {
                app.connection_state = ConnectionState::Error;
            }
            if (event.message.starts_with("Account stream connection failed") ||
                event.message.starts_with("Account WebSocket upgrade failed") ||
                event.message.starts_with(
                    "Account stream authorization failed") ||
                event.message == "Account stream disconnected")
                app.account_stream_failed = true;
            AddMessage(app, std::move(event.message));
            continue;
        }
        if (event.type == UiEventType::MarketClock) {
            const bool first_sync = !app.has_market_clock;
            app.market_clock = event.market_clock;
            app.has_market_clock = true;
            if (first_sync) AddMessage(app, "New York market clock synchronized");
            continue;
        }
        if (event.type == UiEventType::AccountStreamConnected) {
            app.account_stream_latency_ms = event.latency_ms;
            app.account_stream_failed = false;
            AddMessage(app, "Account trade_updates subscription active");
            continue;
        }
        if (event.type == UiEventType::AccountStreamEvent) {
            app.last_account_stream_event_at_ms = event.received_at_ms;
            app.account_event_latency_ms = event.latency_ms;
            continue;
        }
        if (event.type == UiEventType::OrderCommandCompleted) {
            app.command_in_flight = false;
            const auto& result = event.command_result;
            if (result.outcome == tradebox::core::OrderCommandOutcome::Indeterminate)
                app.command_status = "indeterminate: " + result.message;
            else
                app.command_status = result.message.empty()
                    ? "order command completed"
                    : result.message;
            AddMessage(app, "Order " + event.request_id + ": " + app.command_status);
            continue;
        }
        if (event.type == UiEventType::AssetCatalogReady) {
            app.asset_catalog = std::move(event.assets);
            app.database.SaveAssetCatalog(app.asset_catalog);
            AddMessage(app, "Tradable asset catalog ready: " +
                                std::to_string(app.asset_catalog.size()) + " assets");
            continue;
        }
        auto found = app.charts.find(event.symbol);
        if (found == app.charts.end()) continue;
        Chart& chart = found->second;
        if (event.type == UiEventType::HistoricalBars) {
            if (event.timeframe != chart.timeframe) continue;
            chart.bars = std::move(event.bars);
            AddMessage(app, event.symbol + ": " +
                                std::to_string(chart.bars.size()) +
                                " daily bars ready");
        } else if (event.type == UiEventType::DailyBar) {
            UpsertDailyBar(chart, event.bar);
        }
    }
    RefreshCoreProjection(app);
}

std::string NextRequestId(App& app) {
    return "gui-" + std::to_string(SystemNowMs()) + "-" +
           std::to_string(app.next_request_id++);
}

std::optional<tradebox::core::NativeOrderRequest> BuildOrderRequest(
    const OrderEntryDraft& draft, std::string& error) {
    const auto errors = ValidateOrderEntry(draft);
    if (!errors.empty()) {
        error = errors.front().field + ": " + errors.front().message;
        return std::nullopt;
    }
    auto amount = tradebox::core::Decimal::Parse(draft.amount);
    if (!amount) { error = "amount: invalid decimal"; return std::nullopt; }
    tradebox::core::NativeOrderRequest request;
    request.asset_class = tradebox::core::AssetClass::Equity;
    request.symbol = draft.symbol;
    request.side = draft.side == "Sell" ? tradebox::core::OrderSide::Sell
                                         : tradebox::core::OrderSide::Buy;
    request.type = draft.type == "Limit" ? tradebox::core::OrderType::Limit
        : draft.type == "Stop" ? tradebox::core::OrderType::Stop
        : draft.type == "Stop-limit" ? tradebox::core::OrderType::StopLimit
                                      : tradebox::core::OrderType::Market;
    request.time_in_force = draft.time_in_force == "Gtc"
        ? tradebox::core::TimeInForce::Gtc : tradebox::core::TimeInForce::Day;
    request.extended_hours = draft.extended_hours;
    if (draft.amount_is_notional) request.notional = *amount;
    else request.qty = *amount;
    if (!draft.limit_price.empty()) request.limit_price = *tradebox::core::Decimal::Parse(draft.limit_price);
    if (!draft.stop_price.empty()) request.stop_price = *tradebox::core::Decimal::Parse(draft.stop_price);
    return request;
}

void SubmitUiCommand(App& app, tradebox::core::NativeOrderCommand command,
                     std::string request_id) {
    if (app.command_in_flight) return;
    app.command_in_flight = true;
    app.command_request_id = request_id;
    auto future = app.application.SubmitOrder(std::move(command));
    app.command_workers.emplace_back([&app, request_id = std::move(request_id),
                                       future = std::move(future)]() mutable {
        UiEvent event;
        event.type = UiEventType::OrderCommandCompleted;
        event.request_id = request_id;
        event.command_result = future.get();
        app.events.Push(std::move(event));
    });
}

std::optional<std::string> DrawSymbolSelector(
    App& app, const char* label, std::array<char, 24>& input,
    std::string& previous_query,
    std::vector<tradebox::core::TradableAsset>& matches,
    int& cursor) {
    const bool submitted = ImGui::InputText(
        label, input.data(), input.size(),
        ImGuiInputTextFlags_CharsUppercase |
            ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string query = NormalizeSymbol(input.data());
    if (query != previous_query) {
        previous_query = query;
        cursor = 0;
        matches =
            query.empty()
                ? std::vector<tradebox::core::TradableAsset>{}
                : tradebox::core::SearchTradableAssets(
                      app.asset_catalog, query, 5);
    }
    if (ImGui::IsItemActive() && !matches.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            cursor = (cursor + 1) %
                     static_cast<int>(matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            cursor =
                (cursor + static_cast<int>(matches.size()) - 1) %
                static_cast<int>(matches.size());
    }
    if ((ImGui::IsItemHovered() || ImGui::IsItemActive()) &&
        !matches.empty()) {
        ImGui::BeginTooltip();
        for (std::size_t index = 0; index < matches.size(); ++index) {
            const auto& asset = matches[index];
            if (static_cast<int>(index) == cursor)
                ImGui::TextColored(
                    ImVec4(.3f, .85f, 1, 1), "> %s",
                    asset.symbol.c_str());
            else
                ImGui::Text("  %s", asset.symbol.c_str());
            ImGui::SameLine(100);
            ImGui::TextDisabled(
                "%s | %s", asset.name.c_str(),
                asset.exchange.c_str());
        }
        ImGui::EndTooltip();
    }
    if (!submitted) return std::nullopt;
    const std::string selected =
        !matches.empty()
            ? matches[static_cast<std::size_t>(cursor)].symbol
            : query;
    if (selected.empty()) return std::nullopt;
    std::snprintf(input.data(), input.size(), "%s", selected.c_str());
    previous_query = selected;
    matches.clear();
    return selected;
}

void SelectTimeSalesSymbol(App& app, const std::string& symbol,
                           bool update_linked_ticket) {
    if (symbol.empty()) return;
    app.selected_symbol = symbol;
    std::snprintf(
        app.time_sales_symbol.data(), app.time_sales_symbol.size(),
        "%s", symbol.c_str());
    app.time_sales_query = symbol;
    app.time_sales_matches.clear();
    if (!update_linked_ticket ||
        app.linked_order_ticket >= app.order_tickets.size())
        return;
    auto& ticket = app.order_tickets[app.linked_order_ticket];
    ticket.draft.symbol = symbol;
    std::snprintf(
        ticket.symbol.data(), ticket.symbol.size(), "%s",
        symbol.c_str());
    ticket.symbol_query = symbol;
    ticket.symbol_matches.clear();
}

void DrawOrderEntry(App& app) {
    if (app.order_tickets.empty()) app.order_tickets.emplace_back();
    for (std::size_t ticket_index = 0; ticket_index < app.order_tickets.size(); ++ticket_index) {
        auto& ticket = app.order_tickets[ticket_index];
        if (!ticket.window_open) continue;
        ticket.draft.name = ticket.name.data();
        ImGui::PushID(static_cast<int>(ticket_index));
        const std::string title = ticket.draft.name + "###order-ticket-" + std::to_string(ticket_index);
        if (!ticket.was_visible) {
            ImGui::SetNextWindowPos(ImVec2(920.0f + static_cast<float>(ticket_index) * 28.0f,
                                           40.0f + static_cast<float>(ticket_index) * 28.0f),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
        }
        ImGui::SetNextWindowSize(ImVec2(360, 460), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &ticket.window_open)) { ImGui::End(); ImGui::PopID(); continue; }
        ticket.was_visible = true;
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            app.linked_order_ticket = ticket_index;
        if (ImGui::InputText("Ticket name", ticket.name.data(), ticket.name.size()))
            ticket.draft.name = ticket.name.data();
    ImGui::SetNextItemWidth(180);
    if (const auto selected = DrawSymbolSelector(
            app, "Symbol", ticket.symbol, ticket.symbol_query,
            ticket.symbol_matches, ticket.symbol_cursor)) {
        ticket.draft.symbol = *selected;
        app.linked_order_ticket = ticket_index;
        SelectTimeSalesSymbol(app, *selected, false);
    }
    const char* sides[] = {"Buy", "Sell"};
    int side_index = ticket.draft.side == "Sell" ? 1 : 0;
    if (ImGui::Combo("Side", &side_index, sides, 2)) ticket.draft.side = sides[side_index];
    ImGui::InputText("Quantity / notional", ticket.amount.data(), ticket.amount.size());
    ImGui::Checkbox("Notional", &ticket.draft.amount_is_notional);
    const char* types[] = {"Market", "Limit", "Stop", "Stop-limit"};
    int type_index = ticket.draft.type == "Limit" ? 1 : ticket.draft.type == "Stop" ? 2 : ticket.draft.type == "Stop-limit" ? 3 : 0;
    if (ImGui::Combo("Type", &type_index, types, 4)) ticket.draft.type = types[type_index];
    if (ticket.draft.type == "Limit" || ticket.draft.type == "Stop-limit")
        ImGui::InputText("Limit price", ticket.limit.data(), ticket.limit.size());
    if (ticket.draft.type == "Stop" || ticket.draft.type == "Stop-limit")
        ImGui::InputText("Stop price", ticket.stop.data(), ticket.stop.size());
    const char* tifs[] = {"Day", "Gtc"};
    int tif_index = ticket.draft.time_in_force == "Gtc" ? 1 : 0;
    if (ImGui::Combo("Time in force", &tif_index, tifs, 2)) ticket.draft.time_in_force = tifs[tif_index];
    ImGui::Checkbox("Extended hours", &ticket.draft.extended_hours);
    const auto& market = MarketView(app, ticket.draft.symbol);
    if (market.latest_quote) {
        ImGui::Text("Bid %s   Ask %s", market.latest_quote->bid_price.ToString().c_str(),
                    market.latest_quote->ask_price.ToString().c_str());
    } else ImGui::TextDisabled("Bid/ask unavailable");
    std::string validation;
    ImGui::BeginDisabled(app.command_in_flight);
    if (ImGui::Button("Send Order", ImVec2(140, 0))) {
        ticket.draft.amount = ticket.amount.data();
        ticket.draft.limit_price = ticket.limit.data();
        ticket.draft.stop_price = ticket.stop.data();
        auto order = BuildOrderRequest(ticket.draft, validation);
        if (!order) app.command_status = validation;
        else {
            const auto snapshot = app.application.Snapshot();
            const std::string request_id = NextRequestId(app);
            SubmitUiCommand(app, tradebox::core::PlaceOrderCommand{
                .context = {.request_id = request_id, .source = "gui",
                            .account_id = snapshot.account ? snapshot.account->id : "",
                            .environment = snapshot.environment,
                            .generation = snapshot.generation,
                            .live_trading_confirmed = false},
                .order = std::move(*order)}, request_id);
            app.command_status = "pending";
        }
    }
    ImGui::EndDisabled();
    if (!app.command_status.empty()) ImGui::TextWrapped("%s", app.command_status.c_str());
    ImGui::End();
        ImGui::PopID();
    }
}

std::string TradeTime(const std::string& timestamp) {
    const std::size_t separator = timestamp.find('T');
    if (separator == std::string::npos ||
        separator + 9 > timestamp.size())
        return timestamp;
    const std::size_t available =
        std::min<std::size_t>(12, timestamp.size() - separator - 1);
    return timestamp.substr(separator + 1, available);
}

void DrawTimeSales(App& app) {
    if (!app.show_time_sales) return;
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("TIME & SALES", &app.show_time_sales)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(170);
    if (const auto selected = DrawSymbolSelector(
            app, "Symbol", app.time_sales_symbol,
            app.time_sales_query, app.time_sales_matches,
            app.time_sales_cursor))
        SelectTimeSalesSymbol(app, *selected, true);

    const auto& market = MarketView(app, app.selected_symbol);
    const bool stale = market.last_received_at_ms > 0 &&
        SystemNowMs() - market.last_received_at_ms > 5000;
    const char* stream = market.stream_status == tradebox::core::MarketStreamStatus::Subscribed
        ? "CONNECTED" : market.stream_status == tradebox::core::MarketStreamStatus::Stale
        ? "STALE" : "DISCONNECTED";
    const char* feed =
        market.feed == tradebox::core::MarketDataFeed::Sip
            ? "SIP"
            : market.feed == tradebox::core::MarketDataFeed::Iex
                  ? "IEX"
                  : "UNKNOWN FEED";
    ImGui::SameLine();
    ImGui::TextColored(stale ? ImVec4(1, .65f, .2f, 1) : ImVec4(.3f, .9f, .5f, 1),
                       "%s | %s | %s", feed, stream,
                       market.trades_subscribed && market.quotes_subscribed ? "SUBSCRIBED" : "NOT SUBSCRIBED");
    if (market.latest_quote) {
        const auto& quote = *market.latest_quote;
        ImGui::Text("Bid %s x %s (%s)    Ask %s x %s (%s)",
                    quote.bid_price.ToString().c_str(), quote.bid_size.ToString().c_str(), quote.bid_exchange.c_str(),
                    quote.ask_price.ToString().c_str(), quote.ask_size.ToString().c_str(), quote.ask_exchange.c_str());
    }
    ImGui::TextDisabled(
        "%zu prints | newest first", market.trades.size());
    if (ImGui::BeginTable("trade-tape", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Price"); ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Exchange"); ImGui::TableSetupColumn("Conditions"); ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(market.trades.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart;
                 row < clipper.DisplayEnd; ++row) {
                const std::size_t index =
                    static_cast<std::size_t>(row);
                const auto& trade = market.trades[index];
                int direction = 0;
                if (index + 1 < market.trades.size()) {
                    const auto& older = market.trades[index + 1];
                    direction =
                        trade.price > older.price
                            ? 1
                            : trade.price < older.price ? -1 : 0;
                }
                const ImVec4 color =
                    trade.corrected
                        ? ImVec4(1, .7f, .25f, 1)
                        : direction > 0
                              ? ImVec4(.25f, .9f, .58f, 1)
                              : direction < 0
                                    ? ImVec4(.96f, .38f, .43f, 1)
                                    : ImVec4(.75f, .85f, 1, 1);
                const std::string time =
                    TradeTime(trade.broker_timestamp);
                const std::string price =
                    trade.price.ToString();
                const std::string size =
                    trade.size.ToString();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(time.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    color, "%s %s",
                    direction > 0 ? "^" : direction < 0 ? "v" : " ",
                    price.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(size.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(trade.exchange.c_str());
                ImGui::TableNextColumn();
                std::string conditions;
                for (const auto& condition : trade.conditions) {
                    if (!conditions.empty()) conditions += ",";
                    conditions += condition;
                }
                ImGui::TextUnformatted(conditions.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DrawConnectionBadge(const App& app) {
    const bool connected =
        app.connection_state == ConnectionState::Streaming ||
        app.connection_state == ConnectionState::AccountAuthenticated;
    const char* label = "DISCONNECTED";
    if (app.connection_state == ConnectionState::Connecting)
        label = "CONNECTING";
    else if (app.connection_state == ConnectionState::AccountAuthenticated)
        label = "ACCOUNT AUTHENTICATED";
    else if (app.connection_state == ConnectionState::Streaming)
        label = "MARKET STREAMING";
    else if (app.connection_state == ConnectionState::Error)
        label = "CONNECTION ERROR";
    ImGui::TextColored(
        connected ? ImVec4(0.28f, 0.88f, 0.52f, 1.0f)
                  : ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
        "%s", label);
}

void ConstrainCurrentWindowToWorkspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 position = ImGui::GetWindowPos();
    if (position.y < viewport->WorkPos.y)
        ImGui::SetWindowPos(
            ImVec2(position.x, viewport->WorkPos.y));
}

void ConnectSavedAccount(App& app, bool paper) {
    AlpacaCredentials credentials;
    std::string error;
    if (CredentialStore::Load(paper, credentials, error)) {
        app.has_account = false;
        app.positions.clear();
        app.orders.clear();
        app.positions_received_at_ms = 0;
        app.orders_received_at_ms = 0;
        app.active_paper = paper;
        app.connection_state = ConnectionState::Connecting;
        app.market_latency_ms = -1;
        app.last_market_received_at_ms = 0;
        app.market_subscription_active = false;
        app.has_market_clock = false;
        app.market_clock = {};
        app.account_stream_latency_ms = -1;
        app.last_account_stream_event_at_ms = 0;
        app.account_event_latency_ms = -1;
        app.account_stream_failed = false;
        const auto core_result = app.application.Connect({
            .environment =
                paper ? tradebox::core::AccountEnvironment::Paper
                      : tradebox::core::AccountEnvironment::Live,
            .api_key = std::move(credentials.key),
            .api_secret = std::move(credentials.secret),
            .market_symbols = app.watchlist,
        });
        if (!core_result || !core_result->accepted) {
            AddMessage(app, !core_result
                                ? core_result.error().message
                                : core_result->message);
            return;
        }
        app.application.RefreshAssetCatalog();
        AddMessage(app, std::string("Connecting ") +
                            (paper ? "paper" : "live") + " account...");
    } else {
        AddMessage(app, error);
        app.credential_paper = paper;
        app.credentials_open = true;
    }
}

void DisconnectAccount(App& app) {
    const auto core_result = app.application.Disconnect();
    if (!core_result)
        AddMessage(app, "Core disconnect failed: " +
                            core_result.error().message);
    app.has_account = false;
    app.account = {};
    app.account_alias.clear();
    app.account_alias_edit.fill('\0');
    app.positions.clear();
    app.orders.clear();
    app.positions_received_at_ms = 0;
    app.orders_received_at_ms = 0;
    app.connection_state = ConnectionState::Disconnected;
    app.market_latency_ms = -1;
    app.last_market_received_at_ms = 0;
    app.market_subscription_active = false;
    app.requested_market_symbols.clear();
    app.has_market_clock = false;
    app.market_clock = {};
    app.account_stream_latency_ms = -1;
    app.last_account_stream_event_at_ms = 0;
    app.account_event_latency_ms = -1;
    app.account_stream_failed = false;
    AddMessage(app, "Disconnected");
}

void DrawAccount(App& app) {
    if (!app.show_account) return;
    if (!ImGui::Begin("ACCOUNT", &app.show_account)) {
        ImGui::End();
        return;
    }
    ConstrainCurrentWindowToWorkspace();
    DrawConnectionBadge(app);
    if (app.has_account) {
        ImGui::Separator();
        ImGui::TextColored(
            app.active_paper ? ImVec4(0.3f, 0.85f, 0.55f, 1)
                             : ImVec4(1.0f, 0.35f, 0.32f, 1),
            app.active_paper ? "PAPER ACCOUNT" : "LIVE ACCOUNT / READ ONLY");
        ImGui::TextDisabled("Local account name");
        ImGui::SetNextItemWidth(
            std::max(120.0f, ImGui::GetContentRegionAvail().x - 100.0f));
        ImGui::InputText("##account-name", app.account_alias_edit.data(),
                         app.account_alias_edit.size());
        ImGui::SameLine();
        const std::string edited_alias = app.account_alias_edit.data();
        ImGui::BeginDisabled(edited_alias.empty() ||
                             edited_alias == app.account_alias);
        if (ImGui::Button("Save", ImVec2(88.0f, 0))) {
            const std::string account_key =
                app.account.id.empty() ? app.account.account_number
                                       : app.account.id;
            app.account_alias = edited_alias;
            app.database.SaveAccountAlias(
                account_key, app.account.account_number, app.account_alias);
            AddMessage(app, "Account name saved locally");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "Alpaca's Trading API does not expose the dashboard nickname. "
                "This workstation name is stored locally for this account ID.");
        ImGui::Text("Account  %s", app.account.account_number.c_str());
        ImGui::Text("Alpaca status  %s", app.account.status.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "ACTIVE is Alpaca's trading-account eligibility state, not "
                "the connection state.");
        ImGui::Text("Equity   $%s",
                    app.account.equity.ToString().c_str());
        const double equity = app.account.equity.ToDisplayDouble();
        const double last_equity =
            app.account.last_equity.ToDisplayDouble();
        const double daily_pl = equity - last_equity;
        const double daily_plpc =
            last_equity != 0
                ? daily_pl / last_equity * 100.0
                : 0;
        ImGui::TextColored(
            daily_pl >= 0 ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                          : ImVec4(0.96f, 0.38f, 0.43f, 1),
            "Today    %+.2f  (%+.2f%%)", daily_pl, daily_plpc);
        ImGui::Text("Cash     $%s", app.account.cash.ToString().c_str());
        ImGui::Text("Buying power  $%s",
                    app.account.buying_power.ToString().c_str());
        ImGui::TextDisabled("Updated %s ago",
                            FormatDelay(SystemNowMs() -
                                        app.account.received_at_ms)
                                .c_str());
    } else {
        ImGui::TextDisabled("Use Menu > Account to connect.");
    }
    ImGui::End();
}

SDL_HitTestResult SDLCALL WindowHitTest(SDL_Window* window,
                                        const SDL_Point* area, void*) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    const bool maximized =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    if (!maximized) {
        const bool left = area->x < kResizeBorder;
        const bool right = area->x >= width - kResizeBorder;
        const bool top = area->y < kResizeBorder;
        const bool bottom = area->y >= height - kResizeBorder;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    }
    if (area->y < g_title_bar_height &&
        area->x >= kInteractiveTitleBarWidth &&
        area->x <
            width - kWindowControlsWidth - kTitleStatusWidth) {
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

enum class IndicatorVisual {
    Disconnected,
    Connecting,
    Connected,
    Delayed
};

std::string FormatDelay(std::int64_t milliseconds) {
    if (milliseconds < 0) return "--";
    if (milliseconds < 1000) return std::to_string(milliseconds) + "ms";
    std::ostringstream value;
    value << std::fixed << std::setprecision(1)
          << static_cast<double>(milliseconds) / 1000.0 << 's';
    return value.str();
}

void DrawStatusIndicator(const char* id, const std::string& text,
                         IndicatorVisual visual, const std::string& tooltip) {
    ImVec4 color;
    if (visual == IndicatorVisual::Connected) {
        color = ImVec4(0.20f, 0.88f, 0.48f, 1.0f);
    } else if (visual == IndicatorVisual::Disconnected) {
        color = ImVec4(0.95f, 0.25f, 0.28f, 1.0f);
    } else {
        float alpha = 1.0f;
        if (visual == IndicatorVisual::Connecting) {
            const float phase = static_cast<float>(SDL_GetTicks() % 1200) /
                                1200.0f;
            alpha = 0.45f + 0.55f *
                                (0.5f + 0.5f *
                                            std::sin(phase * 2.0f * 3.14159265f));
        }
        color = ImVec4(1.0f, 0.72f, 0.16f, alpha);
    }

    ImGui::PushID(id);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    ImGui::InvisibleButton("##health",
                           ImVec2(text_size.x + 10.0f,
                                  ImGui::GetFrameHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(start.x, start.y + 3.0f),
                        ImVec2(start.x + 3.0f,
                               start.y + ImGui::GetFrameHeight() - 3.0f),
                        ImGui::ColorConvertFloat4ToU32(color), 1.0f);
    draw->AddText(ImVec2(start.x + 8.0f,
                         start.y + (ImGui::GetFrameHeight() - text_size.y) * 0.5f),
                  IM_COL32(205, 211, 222, 255), text.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip.c_str());
    ImGui::PopID();
}

std::string FormatCountdown(std::int64_t milliseconds) {
    const std::int64_t total_seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    const std::int64_t days = total_seconds / 86400;
    const std::int64_t hours = (total_seconds / 3600) % 24;
    const std::int64_t minutes = (total_seconds / 60) % 60;
    const std::int64_t seconds = total_seconds % 60;
    char value[48];
    if (days > 0)
        std::snprintf(value, sizeof(value), "%lldd %02lld:%02lld:%02lld",
                      days, hours, minutes, seconds);
    else
        std::snprintf(value, sizeof(value), "%02lld:%02lld:%02lld",
                      total_seconds / 3600, minutes, seconds);
    return value;
}

bool NewYorkSystemTime(std::int64_t epoch_ms, SYSTEMTIME& local) {
    static const DYNAMIC_TIME_ZONE_INFORMATION eastern = [] {
        DYNAMIC_TIME_ZONE_INFORMATION result{};
        for (DWORD index = 0;; ++index) {
            DYNAMIC_TIME_ZONE_INFORMATION candidate{};
            if (EnumDynamicTimeZoneInformation(index, &candidate) ==
                ERROR_NO_MORE_ITEMS)
                break;
            if (std::wcscmp(candidate.TimeZoneKeyName,
                            L"Eastern Standard Time") == 0) {
                result = candidate;
                break;
            }
        }
        return result;
    }();
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm utc_tm{};
    gmtime_s(&utc_tm, &seconds);
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(utc_tm.tm_year + 1900);
    utc.wMonth = static_cast<WORD>(utc_tm.tm_mon + 1);
    utc.wDay = static_cast<WORD>(utc_tm.tm_mday);
    utc.wDayOfWeek = static_cast<WORD>(utc_tm.tm_wday);
    utc.wHour = static_cast<WORD>(utc_tm.tm_hour);
    utc.wMinute = static_cast<WORD>(utc_tm.tm_min);
    utc.wSecond = static_cast<WORD>(utc_tm.tm_sec);
    utc.wMilliseconds = static_cast<WORD>(epoch_ms % 1000);
    return SystemTimeToTzSpecificLocalTimeEx(&eastern, &utc, &local) != FALSE;
}

void DrawPerformanceOverlay(const App& app) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fps = ImGui::GetIO().Framerate;
    const float frame_ms = fps > 0 ? 1000.0f / fps : 0;
    const std::int64_t system_now = SystemNowMs();
    std::int64_t market_now = system_now;
    if (app.has_market_clock)
        market_now = app.market_clock.timestamp_ms +
                     (system_now - app.market_clock.received_at_ms);

    SYSTEMTIME new_york{};
    char clock_label[64] = "NEW YORK  --:--:-- ET";
    if (NewYorkSystemTime(market_now, new_york))
        std::snprintf(clock_label, sizeof(clock_label),
                      "NEW YORK  %02u:%02u:%02u ET", new_york.wHour,
                      new_york.wMinute, new_york.wSecond);

    std::string session_label = "MARKET CLOCK WAITING";
    if (app.has_market_clock) {
        constexpr std::int64_t premarket_before_open =
            5LL * 60 * 60 * 1000 + 30LL * 60 * 1000;
        const std::int64_t premarket_open =
            app.market_clock.next_open_ms - premarket_before_open;
        if (app.market_clock.is_open) {
            session_label =
                "MARKET OPEN  |  CLOSE IN " +
                FormatCountdown(app.market_clock.next_close_ms - market_now);
        } else if (market_now < premarket_open) {
            session_label =
                "MARKET CLOSED  |  PREMARKET IN " +
                FormatCountdown(premarket_open - market_now) +
                "  |  OPEN IN " +
                FormatCountdown(app.market_clock.next_open_ms - market_now);
        } else if (market_now < app.market_clock.next_open_ms) {
            session_label =
                "PREMARKET  |  OPEN IN " +
                FormatCountdown(app.market_clock.next_open_ms - market_now);
        } else {
            session_label = "MARKET CLOSED  |  CLOCK REFRESHING";
        }
    }

    struct UsageSample {
        ULONGLONG process_time = 0;
        ULONGLONG system_time = 0;
        Uint64 sampled_at = 0;
        float total_cpu_percent = 0;
        float core_equivalents = 0;
        std::size_t working_set_bytes = 0;
    };
    static UsageSample usage;
    const auto file_time_value = [](const FILETIME& value) {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    };
    const Uint64 usage_now = SDL_GetTicks();
    if (usage.sampled_at == 0 || usage_now - usage.sampled_at >= 500) {
        FILETIME created{}, exited{}, process_kernel{}, process_user{};
        FILETIME idle{}, system_kernel{}, system_user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited,
                            &process_kernel, &process_user) &&
            GetSystemTimes(&idle, &system_kernel, &system_user)) {
            const ULONGLONG process_time =
                file_time_value(process_kernel) + file_time_value(process_user);
            const ULONGLONG system_time =
                file_time_value(system_kernel) + file_time_value(system_user);
            if (usage.process_time > 0 && system_time > usage.system_time) {
                usage.total_cpu_percent =
                    100.0f * static_cast<float>(
                                 static_cast<double>(
                                     process_time - usage.process_time) /
                                 static_cast<double>(
                                     system_time - usage.system_time));
                const DWORD logical_processors =
                    std::max<DWORD>(1, GetActiveProcessorCount(
                                           ALL_PROCESSOR_GROUPS));
                usage.core_equivalents =
                    usage.total_cpu_percent * logical_processors / 100.0f;
            }
            usage.process_time = process_time;
            usage.system_time = system_time;
        }
        PROCESS_MEMORY_COUNTERS memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &memory,
                                 sizeof(memory)))
            usage.working_set_bytes = memory.WorkingSetSize;
        usage.sampled_at = usage_now;
    }

    char performance_label[160];
    const char* sync_label =
        app.vsync_enabled
            ? (app.swap_interval == -1 ? "VSYNC ADAPTIVE" : "VSYNC ON")
            : (app.maximum_frame_rate > 0
                   ? "FPS CAPPED"
                   : (app.vsync_requested ? "VSYNC FAILED" : "UNCAPPED"));
    std::snprintf(performance_label, sizeof(performance_label),
                  "%s %.0f Hz  |  %.0f FPS  %.2f ms  |  CPU %.1f%% "
                  "(%.2f core)  |  RAM %.0f MB",
                  sync_label, app.display_refresh_hz, fps, frame_ms,
                  usage.total_cpu_percent,
                  usage.core_equivalents,
                  static_cast<double>(usage.working_set_bytes) /
                      (1024.0 * 1024.0));
    const auto storage_size = [](std::uint64_t bytes) {
        constexpr double kMegabyte = 1024.0 * 1024.0;
        constexpr double kGigabyte = 1024.0 * kMegabyte;
        char value[32];
        if (bytes >= static_cast<std::uint64_t>(kGigabyte))
            std::snprintf(value, sizeof(value), "%.2f GB",
                          static_cast<double>(bytes) / kGigabyte);
        else
            std::snprintf(value, sizeof(value), "%.1f MB",
                          static_cast<double>(bytes) / kMegabyte);
        return std::string(value);
    };
    const std::string storage_label =
        "MARKET DATA  |  CANDLES " +
        storage_size(app.market_storage.candlestick_bytes) + " (" +
        std::to_string(app.market_storage.candlestick_rows) +
        ")  |  TICKS " + storage_size(app.market_storage.tick_bytes) + " (" +
        std::to_string(app.market_storage.tick_rows) + ")  |  DISK " +
        storage_size(app.market_storage.database_bytes);
    const std::string writer_label =
        "WRITE-BEHIND  |  QUEUED " +
        std::to_string(app.writer_telemetry.pending_events) +
        "  |  PEAK " +
        std::to_string(app.writer_telemetry.high_water_events) +
        "  |  DROPPED " +
        std::to_string(
            app.writer_telemetry.dropped_market_events +
            app.writer_telemetry.dropped_timeline_events);
    const std::array<std::string_view, 5> lines = {
        clock_label, session_label, performance_label, storage_label,
        writer_label};
    float text_width = 0;
    for (const std::string_view line : lines)
        text_width =
            std::max(text_width, ImGui::CalcTextSize(line.data()).x);
    const float line_height = ImGui::GetTextLineHeight();
    const ImVec2 padding(7, 4);
    const ImVec2 bottom_right(
        viewport->WorkPos.x + viewport->WorkSize.x - 8,
        viewport->WorkPos.y + viewport->WorkSize.y - 8);
    const ImVec2 top_left(
        bottom_right.x - text_width - padding.x * 2,
        bottom_right.y - line_height * static_cast<float>(lines.size()) -
            padding.y * 2);
    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(top_left, bottom_right, IM_COL32(12, 16, 23, 220), 3);
    draw->AddRect(top_left, bottom_right, IM_COL32(55, 66, 82, 220), 3);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const ImU32 color =
            index == 1
                ? (app.has_market_clock && app.market_clock.is_open
                       ? IM_COL32(71, 224, 133, 255)
                       : IM_COL32(235, 191, 92, 255))
                : IM_COL32(190, 202, 220, 255);
        draw->AddText(
            ImVec2(top_left.x + padding.x,
                   top_left.y + padding.y + line_height * index),
            color, lines[index].data());
    }
}

enum class WindowControlIcon {
    Minimize,
    Maximize,
    Restore,
    Close
};

bool DrawWindowControlButton(const char* id, const char* tooltip,
                             WindowControlIcon icon) {
    ImGui::PushID(id);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(40.0f,
                      static_cast<float>(g_title_bar_height - 8));
    const bool pressed = ImGui::InvisibleButton("##button", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || active) {
        const ImU32 background =
            icon == WindowControlIcon::Close
                ? IM_COL32(210, 35, 48, active ? 255 : 225)
                : IM_COL32(72, 82, 101, active ? 210 : 150);
        draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                            background);
    }

    const ImU32 color = IM_COL32(220, 225, 234, 255);
    const float cx = start.x + size.x * 0.5f;
    const float cy = start.y + size.y * 0.5f;
    constexpr float thickness = 1.5f;
    if (icon == WindowControlIcon::Minimize) {
        draw->AddLine(ImVec2(cx - 5, cy + 3), ImVec2(cx + 5, cy + 3), color,
                      thickness);
    } else if (icon == WindowControlIcon::Maximize) {
        draw->AddRect(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), color,
                      1.0f, 0, thickness);
    } else if (icon == WindowControlIcon::Restore) {
        draw->AddRect(ImVec2(cx - 3, cy - 5), ImVec2(cx + 5, cy + 3), color,
                      1.0f, 0, thickness);
        draw->AddLine(ImVec2(cx - 5, cy - 2), ImVec2(cx - 5, cy + 5), color,
                      thickness);
        draw->AddLine(ImVec2(cx - 5, cy + 5), ImVec2(cx + 2, cy + 5), color,
                      thickness);
    } else {
        draw->AddLine(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), color,
                      thickness);
        draw->AddLine(ImVec2(cx + 5, cy - 5), ImVec2(cx - 5, cy + 5), color,
                      thickness);
    }
    if (hovered) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

void DrawConnectionLight(const App& app, float height) {
    const std::int64_t now = SystemNowMs();
    const std::int64_t account_age =
        app.account.received_at_ms > 0 ? now - app.account.received_at_ms : -1;
    const std::int64_t positions_age =
        app.positions_received_at_ms > 0 ? now - app.positions_received_at_ms
                                         : -1;
    const std::int64_t clock_age =
        app.market_clock.received_at_ms > 0
            ? now - app.market_clock.received_at_ms
            : -1;
    const bool restrictions =
        app.has_account &&
        (app.account.account_blocked || app.account.trade_suspended_by_user ||
         app.account.trading_blocked);
    const bool core_live =
        app.core_view.safety_status ==
        tradebox::core::SafetyStatus::Live;
    const bool healthy =
        app.market_subscription_active && core_live &&
        app.has_account && app.has_market_clock &&
        app.core_view.trading_permitted && !restrictions &&
        account_age >= 0 && account_age < 90000 &&
        positions_age >= 0 && positions_age < 90000 &&
        app.orders_received_at_ms > 0 && clock_age >= 0 && clock_age < 90000;
    const bool disconnected =
        app.connection_state == ConnectionState::Disconnected ||
        app.connection_state == ConnectionState::Error;
    const ImU32 color =
        healthy ? IM_COL32(62, 220, 126, 255)
                : (disconnected ? IM_COL32(239, 68, 79, 255)
                                : IM_COL32(245, 184, 63, 255));

    ImGui::PushID("connection-health");
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##light", ImVec2(22.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(start.x + 11.0f, start.y + height * 0.5f);
    draw->AddCircleFilled(center, 5.0f, color, 24);
    draw->AddCircle(center, 6.5f, IM_COL32(15, 19, 27, 255), 24, 1.5f);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(healthy ? "CONNECTION HEALTHY"
                                      : "CONNECTION NEEDS ATTENTION");
        ImGui::Separator();
        ImGui::Text("Market data       %s",
                    app.market_subscription_active ? "SUBSCRIBED"
                                                   : "NOT SUBSCRIBED");
        ImGui::Text("Trading stream    %s",
                    app.core_view.trade_updates_acknowledged
                        ? "ACKNOWLEDGED"
                        : "NOT ACKNOWLEDGED");
        ImGui::Text("Core safety       %s",
                    app.core_view.status_message.c_str());
        ImGui::Text("Account snapshot  %s old",
                    FormatDelay(account_age).c_str());
        ImGui::Text("Positions         %zu / %s old", app.positions.size(),
                    FormatDelay(positions_age).c_str());
        ImGui::Text("Order history     %s (%zu orders)",
                    app.orders_received_at_ms > 0 ? "LOADED" : "NOT LOADED",
                    app.orders.size());
        ImGui::Text("Exchange clock    %s old",
                    FormatDelay(clock_age).c_str());
        ImGui::Separator();
        ImGui::Text("Environment       %s",
                    app.active_paper ? "PAPER" : "LIVE");
        ImGui::Text("Account           %s",
                    app.has_account ? app.account.account_number.c_str()
                                    : "NONE");
        ImGui::Text("Alpaca status     %s",
                    app.has_account ? app.account.status.c_str() : "--");
        if (restrictions)
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.35f, 1.0f),
                               "TRADING RESTRICTION PRESENT");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

bool DrawTitleBar(App& app, SDL_Window* window) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.067f, 0.09f, 1));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::BeginViewportSideBar("##trade-box-title-bar", viewport, ImGuiDir_Up,
                                static_cast<float>(g_title_bar_height), flags);

    const float title_control_height =
        static_cast<float>(g_title_bar_height - 8);
    if (ImGui::Button("Menu", ImVec2(0, title_control_height)))
        ImGui::OpenPopup("workstation-menu");
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 0), ImVec2(420, 520));
    if (ImGui::BeginPopup("workstation-menu")) {
        const bool connected =
            app.connection_state == ConnectionState::AccountAuthenticated ||
            app.connection_state == ConnectionState::Streaming;
        const bool connecting =
            app.connection_state == ConnectionState::Connecting;
        const char* state_label = "OFFLINE";
        if (connecting)
            state_label = "CONNECTING";
        else if (app.connection_state == ConnectionState::AccountAuthenticated)
            state_label = "AUTHENTICATED";
        else if (app.connection_state == ConnectionState::Streaming)
            state_label = "STREAMING";
        else if (app.connection_state == ConnectionState::Error)
            state_label = "ERROR";
        std::string account_label = "Account    ";
        account_label += app.active_paper ? "PAPER" : "LIVE";
        account_label += " - ";
        account_label += state_label;
        if (ImGui::BeginMenu(account_label.c_str())) {
            ImGui::TextDisabled("ALPACA ACCOUNT");
            if (app.has_account)
                ImGui::TextUnformatted(app.account.account_number.c_str());
            else
                ImGui::TextDisabled("No authenticated account");
            ImGui::Separator();
            if (connecting) {
                ImGui::BeginDisabled();
                ImGui::MenuItem("Connecting...");
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Cancel connection"))
                    DisconnectAccount(app);
            } else if (connected) {
                ImGui::BeginDisabled();
                ImGui::MenuItem("Connected", nullptr, true);
                ImGui::EndDisabled();
                if (ImGui::MenuItem(app.active_paper
                                        ? "Switch to live account"
                                        : "Switch to paper account"))
                    ConnectSavedAccount(app, !app.active_paper);
                if (ImGui::MenuItem("Disconnect")) DisconnectAccount(app);
            } else {
                if (ImGui::MenuItem("Connect paper account"))
                    ConnectSavedAccount(app, true);
                if (ImGui::MenuItem("Connect live account"))
                    ConnectSavedAccount(app, false);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add")) {
            if (ImGui::MenuItem("Chart")) {
                for (auto& [symbol, chart] : app.charts) {
                    static_cast<void>(symbol);
                    chart.window_open = true;
                    RequestChartData(app, chart);
                }
            }
            if (ImGui::MenuItem("Order")) app.order_tickets.emplace_back();
            if (ImGui::MenuItem("Time & Sales"))
                app.show_time_sales = true;
            if (ImGui::MenuItem("Watchlist")) app.show_watchlist = true;
            if (ImGui::MenuItem("Account")) app.show_account = true;
            if (ImGui::MenuItem("Orders")) app.show_orders = true;
            if (ImGui::MenuItem("Positions")) app.show_positions = true;
            if (ImGui::MenuItem("Event log")) app.show_event_log = true;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("Lock workstation", "Coming later");
        ImGui::EndDisabled();
        if (ImGui::BeginMenu("Layouts")) {
            if (ImGui::MenuItem("Save current layout")) {
                ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
                AddMessage(app, "Workspace layout saved");
            }
            if (ImGui::MenuItem("Reload saved layout")) {
                ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
                AddMessage(app, "Workspace layout reloaded");
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Settings and credentials"))
            app.credentials_open = true;
        if (ImGui::MenuItem("Screenshot")) {
            app.capture_path = DefaultScreenshotPath(app.database);
            app.capture_requested = true;
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine(0, 4);
    DrawConnectionLight(app, title_control_height);

    const float controls_x = std::max(
        0.0f, ImGui::GetWindowWidth() - static_cast<float>(kWindowControlsWidth));
    std::string account_identity;
    if (app.has_account) {
        std::ostringstream summary;
        summary << (app.active_paper ? "PAPER" : "LIVE") << "  "
                << app.account_alias << "  " << app.account.account_number
                << "   Equity $" << std::fixed << std::setprecision(2)
                << app.account.equity.ToDisplayDouble() << "   Cash $"
                << app.account.cash.ToDisplayDouble() << "   BP $"
                << app.account.buying_power.ToDisplayDouble();
        account_identity = summary.str();
    } else {
        account_identity = app.active_paper ? "PAPER" : "LIVE";
        account_identity += "  NO ACCOUNT";
    }
    const float identity_width =
        ImGui::CalcTextSize(account_identity.c_str()).x;
    const float status_x =
        std::max(ImGui::GetCursorPosX() + 12.0f,
                 controls_x - identity_width - 12.0f);
    ImGui::SameLine(status_x);
    const ImVec2 title_bar_position = ImGui::GetWindowPos();
    ImGui::PushClipRect(
        ImVec2(title_bar_position.x + status_x, title_bar_position.y),
        ImVec2(title_bar_position.x + controls_x - 4.0f,
               title_bar_position.y + g_title_bar_height),
        true);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(
        app.active_paper ? ImVec4(0.55f, 0.68f, 0.86f, 1)
                         : ImVec4(1.0f, 0.52f, 0.28f, 1),
        "%s", account_identity.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(account_identity.c_str());
        if (app.has_account) {
            ImGui::Separator();
            ImGui::TextWrapped(
                "Equity is the marked-to-market account value. Cash is the "
                "cash ledger balance and may be negative in a margin account. "
                "BP is Alpaca's current buying power for opening positions.");
            if (!app.account.multiplier.empty())
                ImGui::Text("Reported multiplier  %sx",
                            app.account.multiplier.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::PopClipRect();

    bool close_requested = false;
    ImGui::SetCursorPos(ImVec2(controls_x, 4));
    if (DrawWindowControlButton("minimize", "Minimize",
                                WindowControlIcon::Minimize))
        SDL_MinimizeWindow(window);
    ImGui::SameLine(0, 0);
    const bool maximized =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    if (DrawWindowControlButton(
            "maximize", maximized ? "Restore" : "Maximize",
            maximized ? WindowControlIcon::Restore
                      : WindowControlIcon::Maximize)) {
        if (maximized)
            SDL_RestoreWindow(window);
        else
            SDL_MaximizeWindow(window);
    }
    ImGui::SameLine(0, 0);
    if (DrawWindowControlButton("close", "Close", WindowControlIcon::Close))
        close_requested = true;

    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return close_requested;
}

void ForgetSavedCredentials(App& app, bool paper) {
    std::string error;
    if (!CredentialStore::Delete(paper, error)) {
        AddMessage(app, error);
        return;
    }
    app.database.ClearLastConnectedAccount(paper);
    if (app.active_paper == paper) DisconnectAccount(app);
    app.key.fill('\0');
    app.secret.fill('\0');
    AddMessage(app, std::string("Forgot saved ") +
                        (paper ? "paper" : "live") +
                        " credentials from Windows Credential Manager");
}

void DrawCredentials(App& app, SDL_Window* window) {
    if (!app.credentials_open) return;
    ImGui::OpenPopup("Settings");
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Settings", &app.credentials_open,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove)) {
        ImGui::SeparatorText("User interface");
        bool presentation_changed = false;
        if (ImGui::Checkbox("Vertical sync", &app.vsync_requested)) {
            app.database.SaveAppSetting(
                "ui.vsync", app.vsync_requested ? "1" : "0");
            presentation_changed = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Maximum FPS (0 = unlimited)",
                            &app.maximum_frame_rate, 30, 120)) {
            app.maximum_frame_rate =
                std::clamp(app.maximum_frame_rate, 0, 10'000);
            app.database.SaveAppSetting(
                "ui.maximum_fps",
                std::to_string(app.maximum_frame_rate));
            presentation_changed = true;
        }
        if (presentation_changed)
            ApplyPresentationSettings(app, window, true);
        ImGui::TextDisabled(
            "Default: VSync off and no frame-rate limit.");

        ImGui::SeparatorText("Alpaca connection");
        ImGui::TextWrapped(
            "Keys are stored in Windows Credential Manager, never in the "
            "workspace database.");
        ImGui::Checkbox("Paper account", &app.credential_paper);
        ImGui::InputText("API key", app.key.data(), app.key.size());
        ImGui::InputText("Secret", app.secret.data(), app.secret.size(),
                         ImGuiInputTextFlags_Password);
        if (ImGui::Button("Save and connect", ImVec2(180, 0))) {
            AlpacaCredentials credentials{app.key.data(), app.secret.data(),
                                          app.credential_paper};
            std::string error;
            if (credentials.key.empty() || credentials.secret.empty()) {
                AddMessage(app, "Both API key and secret are required");
            } else if (!CredentialStore::Save(credentials, error)) {
                AddMessage(app, error);
            } else {
                std::fill(app.secret.begin(), app.secret.end(), '\0');
                app.has_account = false;
                app.positions.clear();
                app.orders.clear();
                app.positions_received_at_ms = 0;
                app.orders_received_at_ms = 0;
                app.active_paper = credentials.paper;
                app.connection_state = ConnectionState::Connecting;
                app.market_latency_ms = -1;
                app.last_market_received_at_ms = 0;
                app.market_subscription_active = false;
                app.account_stream_latency_ms = -1;
                app.last_account_stream_event_at_ms = 0;
                app.account_event_latency_ms = -1;
                app.account_stream_failed = false;
                const auto core_result = app.application.Connect({
                    .environment =
                        credentials.paper
                            ? tradebox::core::AccountEnvironment::Paper
                            : tradebox::core::AccountEnvironment::Live,
                    .api_key = std::move(credentials.key),
                    .api_secret = std::move(credentials.secret),
                    .market_symbols = app.watchlist,
                });
                if (!core_result || !core_result->accepted) {
                    AddMessage(app, !core_result
                                        ? core_result.error().message
                                        : core_result->message);
                } else {
                    app.application.RefreshAssetCatalog();
                    app.credentials_open = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            std::fill(app.secret.begin(), app.secret.end(), '\0');
            app.credentials_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SeparatorText("Saved credentials");
        if (ImGui::Button("Forget saved paper"))
            ImGui::OpenPopup("Forget paper credentials?");
        ImGui::SameLine();
        if (ImGui::Button("Forget saved live"))
            ImGui::OpenPopup("Forget live credentials?");

        if (ImGui::BeginPopupModal("Forget paper credentials?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Remove the saved paper API key and secret from Windows "
                "Credential Manager?");
            if (ImGui::Button("Forget paper", ImVec2(130, 0))) {
                ForgetSavedCredentials(app, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Forget live credentials?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Remove the saved live API key and secret from Windows "
                "Credential Manager?");
            if (ImGui::Button("Forget live", ImVec2(130, 0))) {
                ForgetSavedCredentials(app, false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndPopup();
    }
}

void AddSymbol(App& app, const std::string& symbol) {
    if (symbol.empty()) return;
    if (std::find(app.watchlist.begin(), app.watchlist.end(), symbol) !=
        app.watchlist.end())
        return;
    app.watchlist.push_back(symbol);
    Chart chart{symbol};
    app.charts.emplace(symbol, std::move(chart));
    app.database.SaveWatchlist(app.watchlist);
    app.application.RefreshMarketSymbols(app.watchlist);
}

void RequestChartData(App& app, Chart& chart) {
    app.application.RequestMarketHistory(
        chart.symbol, chart.timeframe);
}

void OpenChart(App& app, Chart& chart) {
    chart.window_open = true;
    RequestChartData(app, chart);
}

void RemoveSymbol(App& app, std::size_t index) {
    if (index >= app.watchlist.size()) return;
    app.charts.erase(app.watchlist[index]);
    app.watchlist.erase(app.watchlist.begin() + static_cast<std::ptrdiff_t>(index));
    app.database.SaveWatchlist(app.watchlist);
    app.application.RefreshMarketSymbols(app.watchlist);
}

void FormatDate(std::int64_t timestamp_ms, char* output, std::size_t size);

void DrawWatchlist(App& app) {
    if (!app.show_watchlist) return;
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 160),
                                        ImGui::GetMainViewport()->WorkSize);
    if (!ImGui::Begin("WATCHLIST", &app.show_watchlist)) {
        ImGui::End();
        return;
    }
    ConstrainCurrentWindowToWorkspace();
    ImGui::SetNextItemWidth(180);
    const bool submitted = ImGui::InputText(
        "##symbol", app.new_symbol.data(), app.new_symbol.size(),
        ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string asset_query =
        NormalizeSymbol(app.new_symbol.data());
    if (asset_query != app.watchlist_asset_query) {
        app.watchlist_asset_query = asset_query;
        app.asset_cursor = 0;
        app.asset_matches =
            asset_query.empty()
                ? std::vector<tradebox::core::TradableAsset>{}
                : tradebox::core::SearchTradableAssets(
                      app.asset_catalog, asset_query, 5);
    }
    if (ImGui::IsItemActive() && !app.asset_matches.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            app.asset_cursor = (app.asset_cursor + 1) % static_cast<int>(app.asset_matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            app.asset_cursor = (app.asset_cursor + static_cast<int>(app.asset_matches.size()) - 1) % static_cast<int>(app.asset_matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            std::snprintf(app.new_symbol.data(), app.new_symbol.size(), "%s",
                          app.asset_matches[static_cast<std::size_t>(app.asset_cursor)].symbol.c_str());
            AddSymbol(app, app.new_symbol.data());
            app.new_symbol.fill('\0');
            app.asset_matches.clear();
        }
    }
    if ((ImGui::IsItemHovered() || ImGui::IsItemActive()) && !app.asset_matches.empty()) {
        ImGui::BeginTooltip();
        for (std::size_t index = 0; index < app.asset_matches.size(); ++index) {
            const auto& asset = app.asset_matches[index];
            if (static_cast<int>(index) == app.asset_cursor) ImGui::TextColored(ImVec4(.3f,.85f,1,1), "> %s", asset.symbol.c_str());
            else ImGui::Text("  %s", asset.symbol.c_str());
            ImGui::SameLine(100); ImGui::TextDisabled("%s | %s", asset.name.c_str(), asset.exchange.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if ((ImGui::Button("+ Add") || submitted)) {
        AddSymbol(app, NormalizeSymbol(app.new_symbol.data()));
        app.new_symbol.fill('\0');
    }
    ImGui::Separator();
    std::size_t remove = app.watchlist.size();
    if (ImGui::BeginTable("watchlist-prices", 4,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Since close",
                                ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##remove",
                                ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < app.watchlist.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            auto found = app.charts.find(app.watchlist[i]);
            Chart* chart =
                found != app.charts.end() ? &found->second : nullptr;
            if (ImGui::Selectable(app.watchlist[i].c_str(), false,
                                  ImGuiSelectableFlags_None) &&
                chart)
                OpenChart(app, *chart);
            if (ImGui::BeginPopupContextItem("watch-symbol-menu")) {
                if (ImGui::MenuItem("Open chart")) {
                    if (chart) OpenChart(app, *chart);
                }
                if (ImGui::MenuItem("Open trade order")) {
                    App::OrderTicket ticket;
                    ticket.draft.symbol = app.watchlist[i];
                    std::snprintf(
                        ticket.symbol.data(), ticket.symbol.size(),
                        "%s", app.watchlist[i].c_str());
                    ticket.symbol_query = app.watchlist[i];
                    ticket.name = {};
                    const std::string label = "Order " + app.watchlist[i];
                    std::copy(label.begin(), label.end(), ticket.name.begin());
                    ticket.draft.name = label;
                    app.order_tickets.push_back(std::move(ticket));
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            const auto market = app.market_views.find(app.watchlist[i]);
            const bool has_live_trade =
                market != app.market_views.end() &&
                !market->second.snapshot.trades.empty();
            const double live_trade =
                has_live_trade
                    ? market->second.snapshot.trades.front()
                          .price.ToDisplayDouble()
                    : 0;
            if (has_live_trade)
                ImGui::Text("%.2f", live_trade);
            else if (chart && !chart->bars.empty()) {
                ImGui::TextDisabled("%.2f", chart->bars.back().close);
                if (ImGui::IsItemHovered()) {
                    char date[32];
                    FormatDate(chart->bars.back().timestamp_ms, date,
                               sizeof(date));
                    ImGui::SetTooltip("Last cached daily close (%s)", date);
                }
            } else
                ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            const double previous_close =
                chart ? PreviousSessionClose(*chart) : 0;
            if (has_live_trade && previous_close > 0) {
                const double change =
                    (live_trade / previous_close - 1.0) * 100.0;
                ImGui::TextColored(
                    change >= 0 ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                                : ImVec4(0.96f, 0.38f, 0.43f, 1),
                    "%+.2f%%", change);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Previous close %.2f", previous_close);
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::PopID();
    }
        ImGui::EndTable();
    }
    if (remove < app.watchlist.size()) RemoveSymbol(app, remove);
    ImGui::End();
}

void FormatDate(std::int64_t timestamp_ms, char* output, std::size_t size) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    std::strftime(output, size, "%Y-%m-%d", &utc);
}

void DrawChartCanvas(Chart& chart) {
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 240.0f);
    size.y = std::max(size.y, 180.0f);
    ImGui::InvisibleButton("chart-canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft);
    const ImVec2 top_left = ImGui::GetItemRectMin();
    const ImVec2 bottom_right = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(top_left, bottom_right, IM_COL32(12, 17, 25, 255));

    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            chart.visible_bars = std::clamp(
                chart.visible_bars - static_cast<int>(wheel * 12), 20, 1000);
        }
    }
    if (chart.bars.empty()) {
        draw->AddText(ImVec2(top_left.x + 16, top_left.y + 16),
                      IM_COL32(145, 155, 170, 255),
                      "Waiting for daily bars...");
        return;
    }

    const int count =
        std::min(chart.visible_bars, static_cast<int>(chart.bars.size()));
    const int first = static_cast<int>(chart.bars.size()) - count;
    double minimum = chart.bars[first].low;
    double maximum = chart.bars[first].high;
    for (int i = first; i < static_cast<int>(chart.bars.size()); ++i) {
        minimum = std::min(minimum, chart.bars[i].low);
        maximum = std::max(maximum, chart.bars[i].high);
    }
    const double padding = std::max((maximum - minimum) * 0.06, 0.01);
    minimum -= padding;
    maximum += padding;

    const float label_width = 66.0f;
    const float plot_width = std::max(size.x - label_width, 1.0f);
    const float plot_height = std::max(size.y - 20.0f, 1.0f);
    auto y = [&](double price) {
        return top_left.y + static_cast<float>((maximum - price) /
                                               (maximum - minimum)) *
                                plot_height;
    };
    for (int line = 0; line <= 5; ++line) {
        const float line_y = top_left.y + plot_height * line / 5.0f;
        draw->AddLine(ImVec2(top_left.x, line_y),
                      ImVec2(top_left.x + plot_width, line_y),
                      IM_COL32(45, 54, 67, 120));
        const double price = maximum - (maximum - minimum) * line / 5.0;
        char label[32];
        std::snprintf(label, sizeof(label), "%.2f", price);
        draw->AddText(ImVec2(top_left.x + plot_width + 5, line_y - 7),
                      IM_COL32(165, 174, 190, 255), label);
    }

    const float step = plot_width / static_cast<float>(count);
    const float body_width = std::max(1.0f, std::min(step * 0.68f, 12.0f));
    for (int index = 0; index < count; ++index) {
        const Bar& bar = chart.bars[first + index];
        const float x = top_left.x + (index + 0.5f) * step;
        const ImU32 color = bar.close >= bar.open
                                ? IM_COL32(53, 211, 143, 255)
                                : IM_COL32(244, 91, 105, 255);
        draw->AddLine(ImVec2(x, y(bar.high)), ImVec2(x, y(bar.low)), color,
                      1.0f);
        float open_y = y(bar.open);
        float close_y = y(bar.close);
        if (std::fabs(open_y - close_y) < 1.0f) close_y = open_y + 1.0f;
        draw->AddRectFilled(
            ImVec2(x - body_width / 2, std::min(open_y, close_y)),
            ImVec2(x + body_width / 2, std::max(open_y, close_y)), color);
    }

    char first_date[16], last_date[16];
    FormatDate(chart.bars[first].timestamp_ms, first_date, sizeof(first_date));
    FormatDate(chart.bars.back().timestamp_ms, last_date, sizeof(last_date));
    draw->AddText(ImVec2(top_left.x, bottom_right.y - 16),
                  IM_COL32(145, 155, 170, 255), first_date);
    const ImVec2 last_size = ImGui::CalcTextSize(last_date);
    draw->AddText(
        ImVec2(top_left.x + plot_width - last_size.x, bottom_right.y - 16),
        IM_COL32(145, 155, 170, 255), last_date);

    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        draw->AddLine(ImVec2(mouse.x, top_left.y),
                      ImVec2(mouse.x, top_left.y + plot_height),
                      IM_COL32(190, 198, 212, 110));
        draw->AddLine(ImVec2(top_left.x, mouse.y),
                      ImVec2(top_left.x + plot_width, mouse.y),
                      IM_COL32(190, 198, 212, 110));
        const int hovered = std::clamp(
            static_cast<int>((mouse.x - top_left.x) / step), 0, count - 1);
        const Bar& bar = chart.bars[first + hovered];
        char date[16], tooltip[192];
        FormatDate(bar.timestamp_ms, date, sizeof(date));
        std::snprintf(tooltip, sizeof(tooltip),
                      "%s\nO %.2f  H %.2f  L %.2f  C %.2f\nVolume %.0f", date,
                      bar.open, bar.high, bar.low, bar.close, bar.volume);
        ImGui::SetTooltip("%s", tooltip);
    }
}

void DrawCharts(App& app) {
    for (const std::string& symbol : app.watchlist) {
        auto found = app.charts.find(symbol);
        if (found == app.charts.end()) continue;
        Chart& chart = found->second;
        if (!chart.window_open) continue;
        const std::string title = symbol + " - " + chart.timeframe + "###chart-" + symbol;
        ImGui::SetNextWindowSize(ImVec2(640, 390), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &chart.window_open)) {
            ImGui::End();
            continue;
        }
        ConstrainCurrentWindowToWorkspace();
        const char* timeframes[] = {"1Day", "1Hour", "1Min"};
        int timeframe_index =
            chart.timeframe == "1Hour"
                ? 1
                : chart.timeframe == "1Min"
                      ? 2 : 0;
        if (ImGui::Combo("Timeframe", &timeframe_index, timeframes, 3)) {
            chart.timeframe = timeframes[timeframe_index];
            RequestChartData(app, chart);
        }
        if (!chart.bars.empty()) {
            const Bar& latest = chart.bars.back();
            const ImVec4 color = latest.close >= latest.open
                                     ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                                     : ImVec4(0.96f, 0.38f, 0.43f, 1);
            ImGui::TextColored(color, "%.2f", latest.close);
            ImGui::SameLine();
            ImGui::TextDisabled("%zu bars | mouse wheel zoom",
                                chart.bars.size());
        } else {
            ImGui::TextDisabled("%s / IEX", chart.timeframe.c_str());
        }
        DrawChartCanvas(chart);
        ImGui::End();
    }
}

void DrawPositions(App& app) {
    if (!app.show_positions) return;
    ImGui::SetNextWindowSize(ImVec2(920, 310), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("POSITIONS", &app.show_positions)) {
        ImGui::End();
        return;
    }
    ConstrainCurrentWindowToWorkspace();
    if (!app.has_account) {
        ImGui::TextDisabled("Connect an Alpaca account to load positions.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%zu open positions | refreshed %s ago",
                        app.positions.size(),
                        FormatDelay(app.positions_received_at_ms > 0
                                        ? SystemNowMs() -
                                              app.positions_received_at_ms
                                        : -1)
                            .c_str());
    if (ImGui::BeginTable(
            "positions-table", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Market value");
        ImGui::TableSetupColumn("Total P/L");
        ImGui::TableSetupColumn("Today P/L");
        ImGui::TableHeadersRow();
        for (const tradebox::core::PositionState& position :
             app.positions) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(position.symbol.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(position.side.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(position.qty.ToString().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                position.avg_entry_price.ToString().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                position.current_price.ToString().c_str());
            ImGui::TableNextColumn();
            ImGui::Text("$%s", position.market_value.ToString().c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(
                position.unrealized_pl >=
                        tradebox::core::Decimal::Zero()
                    ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                    : ImVec4(0.96f, 0.38f, 0.43f, 1),
                "%+.2f  (%+.2f%%)",
                position.unrealized_pl.ToDisplayDouble(),
                position.unrealized_plpc.ToDisplayDouble() * 100.0);
            ImGui::TableNextColumn();
            ImGui::TextColored(
                position.unrealized_intraday_pl >=
                        tradebox::core::Decimal::Zero()
                    ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                    : ImVec4(0.96f, 0.38f, 0.43f, 1),
                "%+.2f  (%+.2f%%)",
                position.unrealized_intraday_pl.ToDisplayDouble(),
                position.unrealized_intraday_plpc.ToDisplayDouble() *
                    100.0);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DrawOrders(App& app) {
    if (!app.show_orders) return;
    ImGui::SetNextWindowSize(ImVec2(980, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ORDERS", &app.show_orders)) {
        ImGui::End();
        return;
    }
    ConstrainCurrentWindowToWorkspace();
    if (!app.has_account) {
        ImGui::TextDisabled("Connect an Alpaca account to load order history.");
        ImGui::End();
        return;
    }
    const tradebox::core::SafetyStatus safety =
        app.core_view.safety_status;
    if (safety == tradebox::core::SafetyStatus::Live &&
        app.orders_received_at_ms > 0) {
        if (app.last_account_stream_event_at_ms > 0) {
            ImGui::TextDisabled(
                "%zu orders | LIVE via trade_updates | last event %s ago",
                app.orders.size(),
                FormatDelay(SystemNowMs() -
                            app.last_account_stream_event_at_ms)
                    .c_str());
        } else {
            ImGui::TextDisabled(
                "%zu orders | LIVE via trade_updates | no events this session",
                app.orders.size());
        }
    } else if (safety == tradebox::core::SafetyStatus::Reconciling ||
               safety == tradebox::core::SafetyStatus::SnapshotLoading ||
               safety == tradebox::core::SafetyStatus::Connecting) {
        ImGui::TextColored(
            ImVec4(0.96f, 0.72f, 0.30f, 1.0f),
            "%zu orders | RECONCILING | %s", app.orders.size(),
            app.core_view.status_message.c_str());
    } else {
        ImGui::TextColored(
            ImVec4(0.96f, 0.56f, 0.30f, 1.0f),
            "%zu orders | STALE %s old | %s",
            app.orders.size(),
            FormatDelay(app.orders_received_at_ms > 0
                            ? SystemNowMs() - app.orders_received_at_ms
                            : -1)
                .c_str(),
            app.core_view.status_message.c_str());
    }
    if (ImGui::BeginTable(
            "orders-table", 10,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Submitted");
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Filled");
        ImGui::TableSetupColumn("Average fill");
        ImGui::TableSetupColumn("TIF");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(app.orders.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart;
                 index < clipper.DisplayEnd; ++index) {
                const tradebox::core::OrderState& order = app.orders[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string submitted =
                    order.submitted_at.size() > 19
                        ? order.submitted_at.substr(0, 19)
                        : order.submitted_at;
                ImGui::TextUnformatted(submitted.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.symbol.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.side.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.type.c_str());
                ImGui::TableNextColumn();
                if (order.qty)
                    ImGui::TextUnformatted(order.qty->ToString().c_str());
                else if (order.notional)
                    ImGui::Text("$%s",
                                order.notional->ToString().c_str());
                else
                    ImGui::TextDisabled("--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.filled_qty.ToString().c_str());
                ImGui::TableNextColumn();
                if (order.filled_avg_price)
                    ImGui::TextUnformatted(
                        order.filled_avg_price->ToString().c_str());
                else
                    ImGui::TextDisabled("--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.time_in_force.c_str());
                ImGui::TableNextColumn();
                const UiOrderState ui_state = UiOrderStateFromCore(order, app.core_view);
                ImGui::TextUnformatted(UiOrderStateLabel(ui_state).c_str());
                ImGui::TableNextColumn();
                const bool cancelable = ui_state == UiOrderState::Accepted ||
                                        ui_state == UiOrderState::Pending;
                ImGui::BeginDisabled(!cancelable || app.command_in_flight);
                if (ImGui::SmallButton("Cancel")) {
                    const auto snapshot = app.application.Snapshot();
                    const std::string request_id = NextRequestId(app);
                    SubmitUiCommand(app, tradebox::core::CancelOrderCommand{
                        .context = {.request_id = request_id, .source = "gui",
                                    .account_id = snapshot.account ? snapshot.account->id : "",
                                    .environment = snapshot.environment,
                                    .generation = snapshot.generation},
                        .order_id = order.id}, request_id);
                    app.command_status = "pending cancel";
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DrawLog(App& app) {
    if (!app.show_event_log) return;
    if (!ImGui::Begin("EVENT LOG", &app.show_event_log)) {
        ImGui::End();
        return;
    }
    ConstrainCurrentWindowToWorkspace();
    ImGui::TextDisabled("Source events are recorded on the replay timeline.");
    ImGui::Separator();
    for (const std::string& message : app.messages)
        ImGui::TextWrapped("%s", message.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
        ImGui::SetScrollHereY(1.0f);
    ImGui::End();
}

int RunApplication(const LaunchOptions& options) {
    Database database;
    std::string database_error;
    if (!database.Open(database_error)) {
        MessageBoxA(nullptr, database_error.c_str(), "Trade Box database error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    WindowPlacement placement = database.LoadWindowPlacement();
    placement.width = std::max(placement.width, 640);
    placement.height = std::max(placement.height, 480);
    int initial_x = SDL_WINDOWPOS_CENTERED;
    int initial_y = SDL_WINDOWPOS_CENTERED;
    const bool placement_valid =
        placement.exists && PlacementTouchesDisplay(placement);
    if (placement_valid) {
        initial_x = placement.x;
        initial_y = placement.y;
    } else {
        placement.maximized = false;
    }
    SDL_PropertiesID window_properties = SDL_CreateProperties();
    SDL_SetStringProperty(window_properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          "Trade Box");
    SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          initial_x);
    SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          initial_y);
    SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          placement.width);
    SDL_SetNumberProperty(window_properties,
                          SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, placement.height);
    SDL_SetBooleanProperty(window_properties,
                           SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    SDL_SetBooleanProperty(window_properties,
                           SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(window_properties,
                           SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    SDL_SetBooleanProperty(
        window_properties,
        SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(window_properties);
    SDL_DestroyProperties(window_properties);
    if (!window) {
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_SetWindowHitTest(window, WindowHitTest, nullptr);
    if (placement.maximized) SDL_MaximizeWindow(window);
    SDL_GL_MakeCurrent(window, gl);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefaultVector();
    ImGui::GetStyle().FontSizeBase = 14.0f;
    ImGui::GetStyle().FontScaleDpi = SDL_GetWindowDisplayScale(window);
    g_title_bar_height = std::max(
        30, static_cast<int>(std::ceil(
                ImGui::GetStyle().FontSizeBase *
                    ImGui::GetStyle().FontScaleDpi +
                10.0f)));
    const std::string ini_path =
        (database.DataDirectory() / "workspace-layout.ini").string();
    io.IniFilename = ini_path.c_str();
    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 150");

    App app(database);
    app.order_tickets.emplace_back();
    const auto* gl_renderer = reinterpret_cast<const char*>(
        glGetString(GL_RENDERER));
    const auto* gl_version = reinterpret_cast<const char*>(
        glGetString(GL_VERSION));
    AddMessage(
        app, "OpenGL renderer: " +
                 std::string(gl_renderer ? gl_renderer : "unknown") +
                 " | " +
                 std::string(gl_version ? gl_version : "unknown"));
    if (const auto value = database.LoadAppSetting("ui.vsync"))
        app.vsync_requested = *value == "1";
    if (const auto value = database.LoadAppSetting("ui.maximum_fps"))
        app.maximum_frame_rate =
            std::clamp(std::atoi(value->c_str()), 0, 10'000);
    ApplyPresentationSettings(app, window, false);
    const Uint64 launched_at = SDL_GetTicks();
    if (options.capture_and_exit) {
        app.capture_requested = true;
        app.capture_path = options.capture_path;
        app.exit_after_capture = true;
        app.frames_before_capture = 3;
    }
    app.watchlist = database.LoadWatchlist();
    app.asset_catalog = {
        {"AMD", "Advanced Micro Devices", "NASDAQ", true, true, true, true, 45'000'000, 7'000'000'000, SystemNowMs()},
        {"MSFT", "Microsoft Corporation", "NASDAQ", true, true, true, true, 22'000'000, 9'000'000'000, SystemNowMs()},
        {"AAPL", "Apple Inc.", "NASDAQ", true, true, true, true, 52'000'000, 11'000'000'000, SystemNowMs()},
        {"QQQ", "Invesco QQQ Trust", "NASDAQ", true, true, true, true, 31'000'000, 15'000'000'000, SystemNowMs()},
    };
    if (const auto cached_assets = database.LoadAssetCatalog(); !cached_assets.empty())
        app.asset_catalog = cached_assets;
    if (app.watchlist.empty()) {
        app.watchlist = {"AMD", "MSFT", "AAPL", "QQQ"};
    } else {
        for (const char* symbol : {"AMD", "MSFT", "AAPL", "QQQ"}) {
            if (app.watchlist.size() >= 4) break;
            if (std::find(app.watchlist.begin(), app.watchlist.end(), symbol) ==
                app.watchlist.end())
                app.watchlist.emplace_back(symbol);
        }
    }
    if (app.watchlist.size() >= 4) {
        database.SaveWatchlist(app.watchlist);
    }
    for (const std::string& symbol : app.watchlist) {
        Chart chart{symbol};
        app.charts.emplace(symbol, std::move(chart));
    }
    AddMessage(app, "Workspace loaded from " +
                        database.DataDirectory().string());
    if (options.connect_paper) {
        ConnectSavedAccount(app, true);
    } else if (const std::optional<bool> previous =
                   database.LoadLastConnectedPaper()) {
        ConnectSavedAccount(app, *previous);
    }

    int normal_x = placement.x, normal_y = placement.y,
        normal_width = placement.width, normal_height = placement.height;
    if (!placement_valid) {
        SDL_GetWindowPosition(window, &normal_x, &normal_y);
        SDL_GetWindowSize(window, &normal_width, &normal_height);
    }
    bool remember_maximized = placement.maximized;
    bool placement_dirty = false;
    Uint64 placement_changed_at = 0;
    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
                ApplyPresentationSettings(app, window, true);
            if (event.type == SDL_EVENT_WINDOW_MAXIMIZED) {
                remember_maximized = true;
                placement_dirty = true;
                placement_changed_at = SDL_GetTicks();
            } else if (event.type == SDL_EVENT_WINDOW_RESTORED) {
                remember_maximized = false;
                placement_dirty = true;
                placement_changed_at = SDL_GetTicks();
            } else if (event.type == SDL_EVENT_WINDOW_MOVED ||
                       event.type == SDL_EVENT_WINDOW_RESIZED ||
                       event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
                if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) ==
                    0) {
                    SDL_GetWindowPosition(window, &normal_x, &normal_y);
                    SDL_GetWindowSize(window, &normal_width, &normal_height);
                    placement_dirty = true;
                    placement_changed_at = SDL_GetTicks();
                }
            }
        }
        if (placement_dirty &&
            SDL_GetTicks() - placement_changed_at >= 500) {
            database.SaveWindowPlacement(
                {normal_x, normal_y, normal_width, normal_height,
                 remember_maximized, true});
            placement_dirty = false;
        }
        if (options.run_for_ms > 0 &&
            SDL_GetTicks() - launched_at >=
                static_cast<Uint64>(options.run_for_ms))
            done = true;
        DrainEvents(app);
        RefreshRequiredMarketSymbols(app);
        RefreshVisibleMarketData(app);
        const Uint64 storage_now = SDL_GetTicks();
        if (app.market_storage_sampled_at == 0 ||
            storage_now - app.market_storage_sampled_at >= 10000) {
            app.market_storage = database.LoadMarketDataStorageUsage();
            app.market_storage_sampled_at = storage_now;
        }
        if (app.writer_telemetry_sampled_at == 0 ||
            storage_now -
                    app.writer_telemetry_sampled_at >=
                500) {
            app.writer_telemetry =
                database.WriterTelemetry();
            app.writer_telemetry_sampled_at = storage_now;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (DrawTitleBar(app, window)) done = true;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 210), ImGuiCond_FirstUseEver);
        DrawAccount(app);
        ImGui::SetNextWindowPos(ImVec2(10, 230), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
        DrawWatchlist(app);
        ImGui::SetNextWindowPos(ImVec2(10, 550), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_FirstUseEver);
        DrawLog(app);
        DrawCredentials(app, window);
        DrawOrderEntry(app);
        DrawTimeSales(app);
        DrawCharts(app);
        ImGui::SetNextWindowPos(ImVec2(420, 10), ImGuiCond_FirstUseEver);
        DrawPositions(app);
        ImGui::SetNextWindowPos(ImVec2(420, 340), ImGuiCond_FirstUseEver);
        DrawOrders(app);
        DrawPerformanceOverlay(app);

        ImGui::Render();
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.035f, 0.043f, 0.060f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (app.capture_requested) {
            const bool waiting_for_connected_smoke_state =
                app.exit_after_capture &&
                (SDL_GetTicks() - launched_at < 6000 ||
                 (SDL_GetTicks() - launched_at < 10000 &&
                  (!app.has_account || app.positions_received_at_ms == 0 ||
                   app.orders_received_at_ms == 0)));
            if (waiting_for_connected_smoke_state) {
                // Let asynchronous account state reach the UI before a
                // command-line framebuffer smoke capture.
            } else if (app.frames_before_capture > 0) {
                --app.frames_before_capture;
            } else {
                std::string error;
                if (CaptureOpenGLFramebuffer(width, height, app.capture_path,
                                             error)) {
                    AddMessage(app, "Framebuffer saved: " +
                                        app.capture_path.string());
                    const nlohmann::json payload = {
                        {"path", app.capture_path.string()},
                        {"width", width},
                        {"height", height},
                    };
                    database.QueueTimelineEvent(
                        "tradebox.ui", "", "framebuffer_capture", "",
                        SystemNowMs(), payload.dump());
                } else {
                    AddMessage(app, "Framebuffer capture failed: " + error);
                }
                app.capture_requested = false;
                if (app.exit_after_capture) done = true;
            }
        }
        SDL_GL_SwapWindow(window);
        PaceSoftwareFrame(app);
    }

    database.SaveWindowPlacement(
        {normal_x, normal_y, normal_width, normal_height, remember_maximized,
         true});
    const auto disconnect_result = app.application.Disconnect();
    static_cast<void>(disconnect_result);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace

LaunchOptions ParseLaunchOptions(int argc, char* argv[]) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--capture-framebuffer" &&
            index + 1 < argc) {
            options.capture_and_exit = true;
            options.capture_path = std::filesystem::u8path(argv[++index]);
        } else if (std::string_view(argv[index]) == "--connect-paper") {
            options.connect_paper = true;
        } else if (std::string_view(argv[index]) == "--run-for-ms" &&
                   index + 1 < argc) {
            options.run_for_ms = std::max(0, std::atoi(argv[++index]));
        }
    }
    return options;
}

int main(int argc, char* argv[]) {
    return RunApplication(ParseLaunchOptions(argc, argv));
}
