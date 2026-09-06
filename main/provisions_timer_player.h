#ifndef PROVISIONS_TIMER_PLAYER_H_
#define PROVISIONS_TIMER_PLAYER_H_
#include <psa/crypto.h>
#include <deque>
#include <functional>
#include <mutex>
#include "provisions_timer_store.h"
namespace provisions::timers {
// All hardware/persistence hooks run only from Initialize/Service on the main task.
// Network and audio callbacks only update this bounded inbox and observed progress.
class Player {
public:
    struct Hooks {
        std::function<bool(uint32_t)> claim;
        std::function<bool(uint32_t)> release;
        std::function<void()> cancel;
        std::function<bool()> drained;
        std::function<bool(uint32_t, uint32_t, const std::vector<uint8_t>&)> queue;
        std::function<bool(const std::string&)> send;
        std::function<void()> wake;
        std::function<void()> began;
        std::function<void()> ended;
    };
    explicit Player(Store& store);
    ~Player();
    void Initialize(Hooks hooks);
    bool OnJson(const cJSON* root, const std::string& session, bool negotiated, uint32_t press);
    bool OnAudio(const std::vector<uint8_t>& packet, const std::string& source_session);
    void OnProgress(uint32_t id, uint32_t ordinal);
    void OnError(uint32_t id);
    void OnDisconnected();
    void Service(const std::string& session, bool negotiated, bool ready, uint32_t press,
                 int64_t now_us);
    bool Fenced() const;
    Snapshot GetSnapshot() const;

private:
    void Fail(Outcome outcome);
    Store& store_;
    Hooks hooks_;
    mutable std::mutex mutex_;
    Record record_;
    Snapshot snapshot_;
    bool occupied_ = false, fault_ = false, persisted_ = false, claimed_ = false;
    bool started_ = false, sentence_ = false, stopped_ = false, admitted_ = false;
    bool cancel_pending_ = false, ack_pending_ = false, digest_ok_ = false;
    Outcome requested_ = Outcome::Unknown;
    uint32_t owner_ = 0x80000000u, press_ = 0, received_ = 0, submitted_ = 0, played_ = 0;
    int64_t deadline_us_ = 0, last_send_us_ = 0;
    std::string sent_session_;
    std::deque<std::vector<uint8_t>> packets_;
    psa_hash_operation_t digest_ = PSA_HASH_OPERATION_INIT;
};
}  // namespace provisions::timers
#endif
