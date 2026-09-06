#include "provisions_timer_player.h"
#include <cstring>
namespace provisions::timers {
Player::Player(Store& store) : store_(store) {}
Player::~Player() { psa_hash_abort(&digest_); }
void Player::Initialize(Hooks hooks) {
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_ = std::move(hooks);
    if (psa_crypto_init() != PSA_SUCCESS) {
        fault_ = occupied_ = true;
        claimed_ = hooks_.claim(owner_);
        return;
    }
    const auto loaded = store_.Load(record_);
    occupied_ = loaded != Store::LoadResult::Empty;
    fault_ = loaded == Store::LoadResult::Fault;
    if (occupied_) {
        claimed_ = hooks_.claim(owner_);
        fault_ = fault_ || !claimed_;
        persisted_ = loaded == Store::LoadResult::Present;
        // Reboot cannot prove all packets played. Close the initialized output,
        // then persist an interrupted fact for this original lease.
        if (persisted_ && record_.outcome == Outcome::Unknown)
            Fail(Outcome::Interrupted);
    }
}
void Player::Fail(Outcome outcome) {
    if (!occupied_ || record_.outcome != Outcome::Unknown)
        return;
    if (requested_ == Outcome::Unknown)
        requested_ = outcome;
    packets_.clear();
    cancel_pending_ = claimed_;
}
bool Player::Fenced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return occupied_ || fault_;
}
bool Player::OwnsOutput() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return claimed_;
}
Snapshot Player::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}
bool Player::OnJson(const cJSON* root, const std::string& session, bool negotiated,
                    uint32_t press) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!negotiated)
        return false;
    const auto type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type))
        return false;
    if (std::strcmp(type->valuestring, "tts") == 0) {
        std::string state;
        if (!occupied_ || fault_ || record_.outcome != Outcome::Unknown ||
            session != record_.alarm.session_id || !MatchesTts(root, record_.alarm, state))
            return false;
        if (state == "start" && !started_ && !stopped_)
            started_ = true;
        else if (state == "sentence_start" && started_ && !sentence_ && !stopped_)
            sentence_ = true;
        else if (state == "stop" && started_ && sentence_ && !stopped_) {
            stopped_ = true;
            unsigned char bytes[32]{};
            size_t bytes_written = 0;
            char hex[65];
            const bool hashed =
                psa_hash_finish(&digest_, bytes, sizeof(bytes), &bytes_written) == PSA_SUCCESS &&
                bytes_written == sizeof(bytes);
            constexpr char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < 32; ++i) {
                hex[i * 2] = digits[bytes[i] >> 4];
                hex[i * 2 + 1] = digits[bytes[i] & 15];
            }
            hex[64] = '\0';
            digest_ok_ = hashed && received_ == record_.alarm.packet_count &&
                         record_.alarm.audio_sha256 == hex;
            if (!digest_ok_)
                Fail(Outcome::Failed);
        } else {
            Fail(Outcome::Failed);
            return false;
        }
    } else {
        const auto action = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsString(action))
            return false;
        if (std::strcmp(action->valuestring, "snapshot") == 0) {
            Snapshot parsed;
            if (!ParseSnapshot(root, parsed) || parsed.session_id != session)
                return false;
            snapshot_ = std::move(parsed);
        } else if (std::strcmp(action->valuestring, "alarm") == 0) {
            Alarm alarm;
            if (occupied_ || fault_ || owner_ == 0xffffffffu || !ParseAlarm(root, alarm) ||
                alarm.session_id != session)
                return false;
            record_ = {std::move(alarm), Outcome::Unknown};
            occupied_ = true;
            ++owner_;
            press_ = press;
            persisted_ = claimed_ = started_ = sentence_ = stopped_ = admitted_ = false;
            cancel_pending_ = ack_pending_ = digest_ok_ = false;
            requested_ = Outcome::Unknown;
            received_ = submitted_ = played_ = 0;
            deadline_us_ = last_send_us_ = 0;
            sent_session_.clear();
            psa_hash_abort(&digest_);
            if (psa_hash_setup(&digest_, PSA_ALG_SHA_256) != PSA_SUCCESS)
                Fail(Outcome::Failed);
        } else if (std::strcmp(action->valuestring, "drain_ack") == 0) {
            if (!occupied_ || !persisted_ || record_.outcome == Outcome::Unknown ||
                sent_session_ != session || !MatchesAck(root, record_.alarm, session))
                return false;
            ack_pending_ = true;
        } else
            return false;
    }
    if (hooks_.wake)
        hooks_.wake();
    return true;
}
bool Player::OnAudio(const std::vector<uint8_t>& packet, const std::string& source_session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!occupied_)
        return false;
    if (source_session != record_.alarm.session_id || fault_ ||
        record_.outcome != Outcome::Unknown || requested_ != Outcome::Unknown)
        return true;
    if (!started_ || !sentence_ || stopped_ || received_ >= record_.alarm.packet_count ||
        !IsSixtyMsOpus(packet) || packets_.size() >= 20) {
        Fail(Outcome::Failed);
    } else {
        const uint8_t size[2] = {static_cast<uint8_t>(packet.size()),
                                 static_cast<uint8_t>(packet.size() >> 8)};
        if (psa_hash_update(&digest_, size, 2) != 0 ||
            psa_hash_update(&digest_, packet.data(), packet.size()) != 0)
            Fail(Outcome::Failed);
        else {
            ++received_;
            packets_.push_back(packet);
        }
    }
    if (hooks_.wake)
        hooks_.wake();
    return true;
}
void Player::OnProgress(uint32_t id, uint32_t ordinal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!occupied_ || id != owner_ || record_.outcome != Outcome::Unknown)
        return;
    if (ordinal != played_ + 1 || ordinal > submitted_)
        Fail(Outcome::Failed);
    else
        ++played_;
    if (hooks_.wake)
        hooks_.wake();
}
void Player::OnError(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (occupied_ && id == owner_) {
        Fail(Outcome::Failed);
        if (hooks_.wake)
            hooks_.wake();
    }
}
void Player::OnDisconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = {};
    if (occupied_)
        Fail(Outcome::Interrupted);
    if (hooks_.wake)
        hooks_.wake();
}
void Player::Service(const std::string& session, bool negotiated, bool ready, uint32_t press,
                     int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!occupied_ || fault_)
        return;
    if (!persisted_) {
        if (!store_.Save(record_)) {
            fault_ = true;
            if (claimed_)
                hooks_.cancel();
            return;
        }
        persisted_ = true;
    }
    if (record_.outcome == Outcome::Unknown) {
        if (press != press_ || !negotiated || session != record_.alarm.session_id)
            Fail(Outcome::Interrupted);
        if (!admitted_ && requested_ == Outcome::Unknown) {
            if (!ready || !hooks_.claim(owner_)) {
                // Refusal owns no speaker/capture work. Preserve the prior
                // queues, generation and state while awaiting actual drain.
                Fail(Outcome::Failed);
            } else {
                claimed_ = admitted_ = true;
                deadline_us_ = now_us + 45000000;
                hooks_.cancel();
                hooks_.began();
            }
        }
        if (deadline_us_ != 0 && now_us >= deadline_us_)
            Fail(Outcome::Failed);
        if (cancel_pending_) {
            hooks_.cancel();
            cancel_pending_ = false;
        }
        if (requested_ == Outcome::Unknown && admitted_) {
            // Each main-loop pass transfers bounded pending packets; the existing
            // decoder queue provides backpressure without blocking a callback.
            while (!packets_.empty()) {
                if (!hooks_.queue(owner_, submitted_ + 1, packets_.front()))
                    break;
                ++submitted_;
                packets_.pop_front();
            }
        }
        if ((requested_ != Outcome::Unknown || (stopped_ && packets_.empty())) &&
            hooks_.drained()) {
            const Outcome terminal =
                requested_ != Outcome::Unknown ? requested_
                : digest_ok_ && played_ == record_.alarm.packet_count && submitted_ == played_
                    ? Outcome::Completed
                    : Outcome::Failed;
            const Record closed{record_.alarm, terminal};
            if (!store_.Save(closed)) {
                fault_ = true;
                return;
            }
            record_ = closed;
            if (claimed_)
                hooks_.ended();
        }
    }
    if (record_.outcome == Outcome::Unknown)
        return;
    if (ack_pending_) {
        if (!store_.Erase(record_) || (claimed_ && !hooks_.release(owner_))) {
            fault_ = true;
            return;
        }
        occupied_ = persisted_ = claimed_ = ack_pending_ = false;
        packets_.clear();
        return;
    }
    if (negotiated && !session.empty() &&
        (session != sent_session_ || now_us - last_send_us_ >= 1000000)) {
        const auto receipt = ReceiptJson(record_, session);
        if (!receipt.empty() && hooks_.send(receipt)) {
            sent_session_ = session;
            last_send_us_ = now_us;
        }
    }
}
}  // namespace provisions::timers
