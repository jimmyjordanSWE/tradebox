#include "tradebox/broker/alpaca_rest_transport.h"
#include "tradebox/broker/alpaca_payload_limits.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>

namespace tradebox::broker::alpaca {
namespace {

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr,
        0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

void ClearWide(std::wstring& value) noexcept {
    if (!value.empty())
        SecureZeroMemory(
            value.data(), value.size() * sizeof(wchar_t));
    value.clear();
}

std::optional<std::int64_t> HeaderInteger(
    HINTERNET request, const wchar_t* name) {
    DWORD size = 0;
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_CUSTOM, name, nullptr, &size,
        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return std::nullopt;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(
            request, WINHTTP_QUERY_CUSTOM, name, value.data(), &size,
            WINHTTP_NO_HEADER_INDEX))
        return std::nullopt;
    while (!value.empty() &&
           (value.back() == L'\0' || value.back() == L'\r' ||
            value.back() == L'\n'))
        value.pop_back();
    try {
        return std::stoll(value);
    } catch (...) {
        return std::nullopt;
    }
}

bool ApplySecureRequestPolicy(HINTERNET request, DWORD& error) {
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(
            request, WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy, sizeof(redirect_policy))) {
        error = GetLastError();
        return false;
    }
    DWORD enabled_features = WINHTTP_ENABLE_SSL_REVOCATION;
    if (!WinHttpSetOption(
            request, WINHTTP_OPTION_ENABLE_FEATURE,
            &enabled_features, sizeof(enabled_features))) {
        error = GetLastError();
        return false;
    }
    return true;
}

class WinHttpRestExecutor final : public IRestExecutor {
public:
    WinHttpRestExecutor() {
        session_ = WinHttpOpen(
            L"TradeBoxNative/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_)
            WinHttpSetTimeouts(session_, 5000, 5000, 10000, 10000);
    }

    ~WinHttpRestExecutor() override {
        for (const auto& [host, connection] : connections_) {
            static_cast<void>(host);
            if (connection) WinHttpCloseHandle(connection);
        }
        if (session_) WinHttpCloseHandle(session_);
    }

    RestResponse Execute(const RestRequest& input) override {
        RestResponse result;
        if (!session_) {
            result.error = "WinHTTP session is unavailable";
            return result;
        }
        const std::wstring host = Wide(input.host);
        HINTERNET connection = nullptr;
        {
            std::scoped_lock lock(connections_mutex_);
            auto& cached = connections_[input.host];
            if (!cached)
                cached = WinHttpConnect(
                    session_, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            connection = cached;
        }
        const std::wstring method = Wide(input.method);
        const std::wstring path = Wide(input.path);
        HINTERNET request =
            connection
                ? WinHttpOpenRequest(
                      connection, method.c_str(), path.c_str(), nullptr,
                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                      WINHTTP_FLAG_SECURE)
                : nullptr;
        std::wstring key = Wide(input.api_key.Value());
        std::wstring secret = Wide(input.api_secret.Value());
        std::wstring headers = L"APCA-API-KEY-ID: ";
        headers += key;
        headers += L"\r\nAPCA-API-SECRET-KEY: ";
        headers += secret;
        headers += L"\r\nContent-Type: application/json\r\n";
        ClearWide(key);
        ClearWide(secret);
        void* body =
            input.body.empty()
                ? WINHTTP_NO_REQUEST_DATA
                : const_cast<char*>(input.body.data());
        DWORD policy_error = NO_ERROR;
        if (!request) {
            result.error =
                "HTTPS request setup failed (" +
                std::to_string(GetLastError()) + ")";
        } else if (!ApplySecureRequestPolicy(
                       request, policy_error)) {
            result.error =
                "HTTPS security policy failed (" +
                std::to_string(policy_error) + ")";
        } else {
            const bool sent = WinHttpSendRequest(
                request, headers.c_str(), static_cast<DWORD>(-1L), body,
                static_cast<DWORD>(input.body.size()),
                static_cast<DWORD>(input.body.size()), 0);
            ClearWide(headers);
            const bool received =
                sent && WinHttpReceiveResponse(request, nullptr);
            if (!received) {
                result.error =
                    "HTTPS request failed (" +
                    std::to_string(GetLastError()) + ")";
            } else {
            DWORD size = sizeof(result.status);
            WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &size,
                WINHTTP_NO_HEADER_INDEX);
            if (const auto value =
                    HeaderInteger(request, L"X-RateLimit-Limit"))
                result.rate_limit = *value;
            if (const auto value =
                    HeaderInteger(request, L"X-RateLimit-Remaining"))
                result.rate_remaining = *value;
            if (const auto value =
                    HeaderInteger(request, L"X-RateLimit-Reset"))
                result.rate_reset_epoch_seconds = *value;
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    result.status = 0;
                    result.error =
                        "HTTPS response read failed (" +
                        std::to_string(GetLastError()) + ")";
                    result.body.clear();
                    break;
                }
                if (available == 0)
                    break;
                if (result.body.size() >
                        kMaximumRestResponseBytes ||
                    static_cast<std::size_t>(available) >
                        kMaximumRestResponseBytes -
                            result.body.size()) {
                    result.status = 0;
                    result.error =
                        "HTTPS response exceeded " +
                        std::to_string(
                            kMaximumRestResponseBytes) +
                        "-byte safety limit";
                    result.body.clear();
                    break;
                }
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(
                        request, chunk.data(), available, &read)) {
                    result.status = 0;
                    result.error =
                        "HTTPS response read failed (" +
                        std::to_string(GetLastError()) + ")";
                    result.body.clear();
                    break;
                }
                if (!AppendInboundPayload(
                        result.body,
                        std::string_view(chunk.data(), read),
                        kMaximumRestResponseBytes)) {
                    result.status = 0;
                    result.error =
                        "HTTPS response exceeded " +
                        std::to_string(
                            kMaximumRestResponseBytes) +
                        "-byte safety limit";
                    result.body.clear();
                    break;
                }
            }
            }
        }
        ClearWide(headers);
        if (request) WinHttpCloseHandle(request);
        return result;
    }

private:
    HINTERNET session_ = nullptr;
    std::mutex connections_mutex_;
    std::unordered_map<std::string, HINTERNET> connections_;
};

std::string CoalesceKey(const RestRequest& request) {
    if (!request.coalesce || request.method != "GET") return {};
    std::uint64_t credential_fingerprint =
        14695981039346656037ULL;
    for (const unsigned char value :
         request.api_key.Value()) {
        credential_fingerprint ^= value;
        credential_fingerprint *= 1099511628211ULL;
    }
    return std::to_string(static_cast<int>(request.domain)) + "|" +
           request.host + "|" + request.path + "|" +
           std::to_string(credential_fingerprint);
}

std::size_t PriorityIndex(RestPriority priority) {
    return static_cast<std::size_t>(priority);
}

}  // namespace

struct AlpacaRestTransport::Pending {
    RestRequest request;
    std::promise<RestResponse> promise;
    std::shared_future<RestResponse> future;
    std::string coalesce_key;
    std::chrono::steady_clock::time_point not_before;
    std::uint64_t sequence = 0;
    std::uint64_t cancel_generation = 0;
    std::uint32_t attempts = 0;
};

std::unique_ptr<IRestExecutor> CreateWinHttpRestExecutor() {
    return std::make_unique<WinHttpRestExecutor>();
}

AlpacaRestTransport::AlpacaRestTransport(
    std::unique_ptr<IRestExecutor> executor, std::size_t worker_count,
    std::size_t queue_capacity)
    : executor_(std::move(executor)),
      worker_count_(std::clamp<std::size_t>(worker_count, 1, 16)),
      queue_capacity_(std::max<std::size_t>(1, queue_capacity)) {
    workers_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index)
        workers_.emplace_back(&AlpacaRestTransport::WorkerLoop, this);
}

AlpacaRestTransport::~AlpacaRestTransport() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        health_.stopping = true;
        ++cancel_generation_;
        for (auto& queue : queues_) {
            while (!queue.empty()) {
                auto pending = std::move(queue.front());
                queue.pop_front();
                FinishLocked(
                    pending,
                    {.error = "REST transport is shutting down"});
                ++health_.canceled;
            }
        }
        health_.queued = 0;
    }
    ready_.notify_all();
    for (auto& worker : workers_)
        if (worker.joinable()) worker.join();
}

std::shared_future<RestResponse> AlpacaRestTransport::Submit(
    RestRequest request) {
    auto pending = std::make_shared<Pending>();
    pending->request = std::move(request);
    pending->future = pending->promise.get_future().share();
    pending->not_before = std::chrono::steady_clock::now();
    pending->coalesce_key = CoalesceKey(pending->request);
    std::scoped_lock lock(mutex_);
    if (stopping_) {
        pending->promise.set_value(
            {.error = "REST transport is shutting down"});
        ++health_.rejected;
        return pending->future;
    }
    if (!pending->coalesce_key.empty()) {
        const auto found = coalesced_.find(pending->coalesce_key);
        if (found != coalesced_.end()) {
            if (auto existing = found->second.lock()) {
                ++health_.coalesced;
                return existing->future;
            }
            coalesced_.erase(found);
        }
    }
    if (health_.queued >= queue_capacity_) {
        pending->promise.set_value(
            {.error = "REST request queue is full"});
        ++health_.rejected;
        return pending->future;
    }
    pending->sequence = next_sequence_++;
    pending->cancel_generation = cancel_generation_;
    queues_[PriorityIndex(pending->request.priority)].push_back(pending);
    if (!pending->coalesce_key.empty())
        coalesced_[pending->coalesce_key] = pending;
    ++health_.queued;
    health_.queue_high_water =
        std::max(health_.queue_high_water, health_.queued);
    ready_.notify_one();
    return pending->future;
}

RestResponse AlpacaRestTransport::Execute(RestRequest request) {
    return Submit(std::move(request)).get();
}

void AlpacaRestTransport::CancelPending() {
    std::scoped_lock lock(mutex_);
    ++cancel_generation_;
    for (auto& queue : queues_) {
        while (!queue.empty()) {
            auto pending = std::move(queue.front());
            queue.pop_front();
            FinishLocked(pending, {.error = "REST request canceled"});
            ++health_.canceled;
            --health_.queued;
        }
    }
    ready_.notify_all();
}

tradebox::core::RestTransportHealth AlpacaRestTransport::Health() const {
    std::scoped_lock lock(mutex_);
    return health_;
}

void AlpacaRestTransport::FinishLocked(
    const std::shared_ptr<Pending>& pending, RestResponse response) {
    if (!pending->coalesce_key.empty()) {
        const auto found = coalesced_.find(pending->coalesce_key);
        if (found != coalesced_.end()) coalesced_.erase(found);
    }
    pending->promise.set_value(std::move(response));
}

tradebox::core::RestRateBudget& AlpacaRestTransport::Budget(
    RestDomain domain) {
    return domain == RestDomain::Trading ? health_.trading
                                         : health_.market_data;
}

const tradebox::core::RestRateBudget& AlpacaRestTransport::Budget(
    RestDomain domain) const {
    return domain == RestDomain::Trading ? health_.trading
                                         : health_.market_data;
}

std::int64_t AlpacaRestTransport::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void AlpacaRestTransport::WorkerLoop() {
    for (;;) {
        std::shared_ptr<Pending> pending;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (stopping_) return;
                const auto now = std::chrono::steady_clock::now();
                auto earliest =
                    std::chrono::steady_clock::time_point::max();
                for (auto& queue : queues_) {
                    for (auto iterator = queue.begin();
                         iterator != queue.end(); ++iterator) {
                        const bool command =
                            (*iterator)->request.priority ==
                            RestPriority::Command;
                        const std::size_t non_command_limit =
                            worker_count_ > 1 ? worker_count_ - 1 : 1;
                        if (!command &&
                            non_command_in_flight_ >=
                                non_command_limit)
                            continue;
                        auto ready_at = (*iterator)->not_before;
                        const tradebox::core::RestRateBudget& budget =
                            Budget((*iterator)->request.domain);
                        if (budget.remaining == 0 &&
                            budget.reset_at_ms > NowMs()) {
                            ready_at = std::max(
                                ready_at,
                                now + std::chrono::milliseconds(
                                          budget.reset_at_ms - NowMs()));
                        }
                        if (ready_at <= now) {
                            pending = *iterator;
                            queue.erase(iterator);
                            break;
                        }
                        earliest = std::min(earliest, ready_at);
                    }
                    if (pending) break;
                }
                if (pending) break;
                if (health_.queued == 0)
                    ready_.wait(lock);
                else
                    ready_.wait_until(lock, earliest);
            }
            --health_.queued;
            ++health_.in_flight;
            if (pending->request.priority != RestPriority::Command)
                ++non_command_in_flight_;
            tradebox::core::RestRateBudget& budget =
                Budget(pending->request.domain);
            if (budget.remaining > 0) --budget.remaining;
        }

        RestResponse response =
            executor_->Execute(pending->request);

        std::scoped_lock lock(mutex_);
        --health_.in_flight;
        if (pending->request.priority != RestPriority::Command)
            --non_command_in_flight_;
        ready_.notify_all();
        tradebox::core::RestRateBudget& budget =
            Budget(pending->request.domain);
        if (response.rate_limit >= 0)
            budget.limit = response.rate_limit;
        if (response.rate_remaining >= 0)
            budget.remaining = response.rate_remaining;
        if (response.rate_reset_epoch_seconds > 0)
            budget.reset_at_ms =
                response.rate_reset_epoch_seconds * 1000;
        const bool retryable_response =
            !response.error.empty() || response.status == 408 ||
            response.status == 429 || response.status >= 500;
        if (pending->request.safe_to_retry && retryable_response &&
            pending->attempts < 2 && !stopping_ &&
            pending->cancel_generation == cancel_generation_) {
            ++pending->attempts;
            ++health_.retries;
            std::int64_t delay_ms =
                50LL << (pending->attempts - 1);
            delay_ms +=
                static_cast<std::int64_t>(pending->sequence % 17);
            if (response.status == 429 &&
                budget.reset_at_ms > NowMs())
                delay_ms = std::max(
                    delay_ms, budget.reset_at_ms - NowMs());
            pending->not_before =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(delay_ms);
            queues_[PriorityIndex(pending->request.priority)]
                .push_back(pending);
            ++health_.queued;
            ready_.notify_all();
            continue;
        }
        if (pending->cancel_generation != cancel_generation_) {
            response = {.error = "REST request canceled"};
            ++health_.canceled;
        }
        if (response.status >= 400 && response.status < 500)
            ++health_.http_rejected;
        FinishLocked(pending, std::move(response));
        ++health_.completed;
    }
}

}  // namespace tradebox::broker::alpaca
