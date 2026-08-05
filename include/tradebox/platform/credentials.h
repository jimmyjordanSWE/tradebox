#pragma once

#include <string>
#include <string_view>
#include <utility>

struct AlpacaCredentials {
    std::string key;
    std::string secret;
    bool paper = true;

    AlpacaCredentials() = default;
    AlpacaCredentials(std::string api_key, std::string api_secret,
                      bool paper_environment)
        : key(api_key),
          secret(api_secret),
          paper(paper_environment) {
        Wipe(api_key);
        Wipe(api_secret);
    }

    AlpacaCredentials(const AlpacaCredentials&) = delete;
    AlpacaCredentials& operator=(const AlpacaCredentials&) = delete;

    AlpacaCredentials(AlpacaCredentials&& other)
        : key(other.key),
          secret(other.secret),
          paper(other.paper) {
        Wipe(other.key);
        Wipe(other.secret);
    }

    AlpacaCredentials& operator=(AlpacaCredentials&& other) {
        if (this == &other) return *this;
        Wipe(key);
        Wipe(secret);
        key = other.key;
        secret = other.secret;
        paper = other.paper;
        Wipe(other.key);
        Wipe(other.secret);
        return *this;
    }

    ~AlpacaCredentials() {
        Wipe(key);
        Wipe(secret);
    }

private:
    static void Wipe(std::string& value) noexcept {
        volatile char* bytes = value.data();
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[index] = '\0';
        value.clear();
    }
};

class CredentialStore {
public:
    static bool Save(std::string_view slot, const AlpacaCredentials& credentials,
                     std::string& error);
    static bool Load(std::string_view slot, bool paper,
                     AlpacaCredentials& credentials, std::string& error);
    static bool Exists(std::string_view slot, bool paper);
    static bool Delete(std::string_view slot, bool paper, std::string& error);
    static bool Save(const AlpacaCredentials& credentials, std::string& error);
    static bool Load(bool paper, AlpacaCredentials& credentials, std::string& error);
    static bool Delete(bool paper, std::string& error);
};
