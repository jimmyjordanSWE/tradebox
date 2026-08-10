#pragma once

#include "tradebox/core/rest_health.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tradebox::broker::alpaca {

class SensitiveString final {
public:
    SensitiveString() = default;
    SensitiveString(const char* value) : value_(value ? value : "") {}
    SensitiveString(const std::string& value) : value_(value) {}
    SensitiveString(std::string&& value)
        : value_(value) {
        ClearValue(value);
    }

    SensitiveString(const SensitiveString& other)
        : value_(other.value_) {}
    SensitiveString& operator=(const SensitiveString& other) {
        if (this == &other) return *this;
        Clear();
        value_ = other.value_;
        return *this;
    }
    SensitiveString(SensitiveString&& other)
        : value_(other.value_) {
        other.Clear();
    }
    SensitiveString& operator=(SensitiveString&& other) {
        if (this == &other) return *this;
        Clear();
        value_ = other.value_;
        other.Clear();
        return *this;
    }
    ~SensitiveString() { Clear(); }

    [[nodiscard]] const std::string& Value() const noexcept {
        return value_;
    }

private:
    void Clear() noexcept {
        ClearValue(value_);
    }

    static void ClearValue(std::string& value) noexcept {
        volatile char* bytes = value.data();
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[index] = '\0';
        value.clear();
    }

    std::string value_;
};

enum class RestDomain { Trading, MarketData };
enum class RestPriority {
    Command,
    AccountSafety,
    Interactive,
    Background,
};

struct RestRequest {
    std::string method = "GET";
    std::string host;
    std::string path;
    SensitiveString api_key;
    SensitiveString api_secret;
    std::string body;
    RestDomain domain = RestDomain::Trading;
    RestPriority priority = RestPriority::Background;
    bool safe_to_retry = true;
    bool coalesce = true;
};

struct RestResponse {
    std::uint32_t status = 0;
    std::string body;
    std::string error;
    std::int64_t rate_limit = -1;
    std::int64_t rate_remaining = -1;
    std::int64_t rate_reset_epoch_seconds = 0;
};

class IRestExecutor {
public:
    virtual ~IRestExecutor() = default;
    virtual RestResponse Execute(const RestRequest& request) = 0;
    virtual void Abort() {}
};

std::unique_ptr<IRestExecutor> CreateWinHttpRestExecutor();

class AlpacaRestTransport final {
public:
    explicit AlpacaRestTransport(
        std::unique_ptr<IRestExecutor> executor =
            CreateWinHttpRestExecutor(),
        std::size_t worker_count = 4,
        std::size_t queue_capacity = 512);
    ~AlpacaRestTransport();

    AlpacaRestTransport(const AlpacaRestTransport&) = delete;
    AlpacaRestTransport& operator=(const AlpacaRestTransport&) = delete;

    std::shared_future<RestResponse> Submit(RestRequest request);
    RestResponse Execute(RestRequest request);
    void CancelPending();
    void Abort();
    void Resume();
    tradebox::core::RestTransportHealth Health() const;

private:
    struct Pending;

    void WorkerLoop();
    void FinishLocked(
        const std::shared_ptr<Pending>& pending,
        RestResponse response);
    tradebox::core::RestRateBudget& Budget(RestDomain domain);
    const tradebox::core::RestRateBudget& Budget(RestDomain domain) const;
    static std::int64_t NowMs();

    std::unique_ptr<IRestExecutor> executor_;
    const std::size_t worker_count_;
    const std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::array<std::deque<std::shared_ptr<Pending>>, 4> queues_;
    std::unordered_map<std::string, std::weak_ptr<Pending>> coalesced_;
    std::vector<std::thread> workers_;
    tradebox::core::RestTransportHealth health_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t cancel_generation_ = 0;
    std::size_t non_command_in_flight_ = 0;
    bool aborted_ = false;
    bool stopping_ = false;
};

}  // namespace tradebox::broker::alpaca
