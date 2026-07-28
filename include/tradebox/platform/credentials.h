#pragma once

#include <string>

struct AlpacaCredentials {
    std::string key;
    std::string secret;
    bool paper = true;

    ~AlpacaCredentials() {
        Wipe(key);
        Wipe(secret);
    }

private:
    static void Wipe(std::string& value) noexcept {
        volatile char* bytes = value.data();
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[index] = '\0';
    }
};

class CredentialStore {
public:
    static bool Save(const AlpacaCredentials& credentials, std::string& error);
    static bool Load(bool paper, AlpacaCredentials& credentials, std::string& error);
    static bool Delete(bool paper, std::string& error);
};
