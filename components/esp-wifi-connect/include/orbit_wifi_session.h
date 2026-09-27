#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

// Independent of ESP-IDF so expiry/save/cancel ordering can be tested on the host.
class OrbitWifiSession {
public:
    enum class Result { Inactive, Active, Saved, Cancelled, Expired };
    static constexpr int64_t kLifetimeUs = 300LL * 1000000;
    void Begin(int64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        deadline_ = now + kLifetimeUs;
        result_ = Result::Active;
    }
    Result Poll(int64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        Expire(now);
        return result_;
    }
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result_ == Result::Active)
            result_ = Result::Cancelled;
    }
    template <typename Save>
    bool Commit(int64_t now, Save save) {
        std::lock_guard<std::mutex> lock(mutex_);
        Expire(now);
        if (result_ != Result::Active)
            return false;
        if (!save())
            return false;
        result_ = Result::Saved;
        return true;
    }
    // The portal accepts one flat object of scalar fields. Reject nested attacker
    // input before cJSON recursion can consume the HTTP task's bounded stack.
    static bool FlatJson(const std::string& body) {
        int depth = 0;
        bool quoted = false, escaped = false;
        for (char c : body) {
            if (quoted) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    quoted = false;
            } else {
                if (c == '"')
                    quoted = true;
                else if (c == '[' || c == ']')
                    return false;
                else if (c == '{' && ++depth > 1)
                    return false;
                else if (c == '}' && --depth < 0)
                    return false;
            }
        }
        return !quoted && depth == 0;
    }
    static bool ValidCredentials(const std::string& ssid, const std::string& password) {
        if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != std::string::npos ||
            password.find('\0') != std::string::npos)
            return false;
        if (password.empty())
            return true;
        if (password.size() >= 8 && password.size() <= 63)
            return true;
        return password.size() == 64 && std::all_of(password.begin(), password.end(), [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F');
               });
    }

private:
    void Expire(int64_t now) {
        if (result_ == Result::Active && now >= deadline_)
            result_ = Result::Expired;
    }
    std::mutex mutex_;
    Result result_ = Result::Inactive;
    int64_t deadline_ = 0;
};
