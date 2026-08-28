#ifndef PROVISIONS_TTS_TURN_H_
#define PROVISIONS_TTS_TURN_H_

#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

class ProvisionsTtsTurn {
public:
    using Token = uint64_t;
    static constexpr Token kInvalidToken = 0;

    enum class Phase : uint8_t {
        kIdle,
        kStarted,
        kSentence,
    };

    enum class Outcome : uint8_t {
        kAccepted,
        kDuplicate,
        kInvalidOrder,
    };

    struct Transition {
        Outcome outcome;
        Token token;
    };

    Transition Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != Phase::kIdle) {
            return {Outcome::kDuplicate, generation_};
        }
        AdvanceGenerationLocked();
        phase_ = Phase::kStarted;
        return {Outcome::kAccepted, generation_};
    }

    Transition Sentence() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::kIdle) {
            return {Outcome::kInvalidOrder, kInvalidToken};
        }
        if (phase_ == Phase::kSentence) {
            return {Outcome::kDuplicate, generation_};
        }
        phase_ = Phase::kSentence;
        return {Outcome::kAccepted, generation_};
    }

    Transition Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::kIdle) {
            return {Outcome::kDuplicate, generation_};
        }
        phase_ = Phase::kIdle;
        return {Outcome::kAccepted, generation_};
    }

    void Invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        AdvanceGenerationLocked();
        phase_ = Phase::kIdle;
    }

    template <typename Action>
    bool WithCurrent(Token token, Action&& action) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == kInvalidToken || token != generation_) {
            return false;
        }
        std::forward<Action>(action)();
        return true;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_;
    }

private:
    void AdvanceGenerationLocked() {
        generation_ = generation_ == std::numeric_limits<Token>::max() ? 1 : generation_ + 1;
    }

    mutable std::mutex mutex_;
    Token generation_ = 0;
    Phase phase_ = Phase::kIdle;
};

#endif  // PROVISIONS_TTS_TURN_H_
