#include "provisions_timer_player.h"

#include <cstring>

namespace provisions::timers {
Player::Player(Store& store) : store_(store) {}
Player::~Player() { psa_hash_abort(&digest_); }

void Player::ClearPreparationIntent() {
    preparation_intent_session_.clear();
    preparation_intent_lease_.clear();
    preparation_intent_press_ = 0;
}

void Player::Initialize(Hooks hooks) {
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_ = std::move(hooks);
    if (psa_crypto_init() != PSA_SUCCESS) {
        fault_ = true;
        return;
    }
    const auto loaded = store_.Load(slot_);
    fault_ = loaded == Store::LoadResult::Fault;
    if (loaded != Store::LoadResult::Present)
        return;
    if (slot_.state == DurableState::Prepared) {
        // A preparation found after boot has an unknown claim result. It must
        // be durably retired before this device can offer another lease.
        preparation_uncertain_ = true;
        return;
    }
    if (slot_.state == DurableState::NoStartPending)
        return;
    if (slot_.state != DurableState::Alarm) {
        fault_ = true;
        return;
    }
    record_ = slot_.record;
    occupied_ = persisted_ = true;
    claimed_ = hooks_.claim(owner_);
    fault_ = !claimed_;
    // Reboot cannot prove all packets played. Close the initialized output,
    // then persist an interrupted fact for this original lease.
    if (!fault_ && record_.outcome == Outcome::Unknown)
        Fail(Outcome::Interrupted);
}

void Player::Fail(Outcome outcome) {
    if ((!occupied_ && !alarm_pending_) || (occupied_ && record_.outcome != Outcome::Unknown))
        return;
    if (requested_ == Outcome::Unknown)
        requested_ = outcome;
    packets_.clear();
    cancel_pending_ = claimed_;
}

bool Player::StageAlarm(Alarm alarm, uint32_t press) {
    pending_record_ = {std::move(alarm), Outcome::Unknown};
    alarm_pending_ = true;
    press_ = press;
    started_ = sentence_ = stopped_ = admitted_ = false;
    cancel_pending_ = ack_pending_ = digest_ok_ = false;
    requested_ = Outcome::Unknown;
    received_ = submitted_ = played_ = 0;
    deadline_us_ = last_send_us_ = 0;
    sent_session_.clear();
    packets_.clear();
    psa_hash_abort(&digest_);
    if (psa_hash_setup(&digest_, PSA_ALG_SHA_256) != PSA_SUCCESS)
        requested_ = Outcome::Failed;
    return true;
}

bool Player::Fenced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Durable recovery blocks publication without owning the microphone or
    // speaker. A physical press may still be captured locally and replayed
    // after the exact recovery ACK clears this slot.
    return slot_.state != DurableState::Empty || fault_;
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
        const Record* active = occupied_ ? &record_ : alarm_pending_ ? &pending_record_ : nullptr;
        std::string state;
        if (!active || fault_ || active->outcome != Outcome::Unknown ||
            session != active->alarm.session_id || !MatchesTts(root, active->alarm, state))
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
            digest_ok_ = hashed && received_ == active->alarm.packet_count &&
                         active->alarm.audio_sha256 == hex;
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
        if (std::strcmp(action->valuestring, "prepare_alarm") == 0) {
            std::string lease_id;
            if (fault_ || !ParsePreparationRequest(root, session, lease_id))
                return false;
            if (slot_.state == DurableState::Empty) {
                if (!preparation_intent_lease_.empty() &&
                    (preparation_intent_lease_ != lease_id ||
                     preparation_intent_session_ != session))
                    return false;
                if (preparation_intent_lease_.empty()) {
                    preparation_intent_lease_ = std::move(lease_id);
                    preparation_intent_session_ = session;
                    preparation_intent_press_ = press;
                }
            } else if (slot_.state == DurableState::Prepared && slot_.lease_id == lease_id) {
                if (!alarm_pending_ && !abandon_pending_) {
                    // An exact request retry proves no new owner. Make the main
                    // task resend the same already-durable preparation immediately.
                    recovery_sent_session_.clear();
                    recovery_last_send_us_ = 0;
                }
            } else {
                return false;
            }
        } else if (std::strcmp(action->valuestring, "snapshot") == 0) {
            Snapshot parsed;
            if (!ParseSnapshot(root, parsed) || parsed.session_id != session)
                return false;
            snapshot_ = std::move(parsed);
        } else if (std::strcmp(action->valuestring, "alarm") == 0) {
            Alarm alarm;
            if (fault_ || owner_ == 0xffffffffu || !ParseAlarm(root, alarm) ||
                alarm.session_id != session)
                return false;
            const Record candidate{alarm, Outcome::Unknown};
            if (occupied_)
                return DurableSlotJson(slot_) ==
                       DurableSlotJson({DurableState::Alarm, alarm.lease_id, candidate});
            if (alarm_pending_)
                return RecordJson(pending_record_) == RecordJson(candidate);
            if (slot_.state != DurableState::Prepared || abandon_pending_ ||
                slot_.lease_id != alarm.lease_id)
                return false;
            StageAlarm(std::move(alarm), press);
        } else if (std::strcmp(action->valuestring, "abandon_preparation") == 0) {
            if (slot_.state == DurableState::Empty && !preparation_intent_lease_.empty() &&
                preparation_intent_session_ == session &&
                MatchesRecoveryRequest(root, "abandon_preparation", preparation_intent_lease_,
                                       session)) {
                ClearPreparationIntent();
                if (hooks_.wake)
                    hooks_.wake();
                return true;
            }
            if ((slot_.state != DurableState::Prepared &&
                 slot_.state != DurableState::NoStartPending &&
                 slot_.state != DurableState::Alarm) ||
                !MatchesRecoveryRequest(root, "abandon_preparation", slot_.lease_id, session))
                return false;
            // An accepted alarm header wins the serialized transition. The
            // request is harmless after either terminal durable state.
            if (slot_.state == DurableState::Prepared && !alarm_pending_)
                abandon_pending_ = true;
        } else if (std::strcmp(action->valuestring, "drain_ack") == 0) {
            if (!occupied_ || !persisted_ || record_.outcome == Outcome::Unknown ||
                sent_session_ != session || !MatchesAck(root, record_.alarm, session))
                return false;
            ack_pending_ = true;
        } else if (std::strcmp(action->valuestring, "no_start_ack") == 0) {
            if (slot_.state != DurableState::NoStartPending || recovery_sent_session_ != session ||
                !MatchesNoStartAck(root, slot_.lease_id, session))
                return false;
            ack_pending_ = true;
        } else {
            return false;
        }
    }
    if (hooks_.wake)
        hooks_.wake();
    return true;
}

bool Player::OnAudio(const std::vector<uint8_t>& packet, const std::string& source_session) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Gateway holder/send-lock ordering forbids ordinary narration until the
    // exact no-start ACK is ordered. Any binary observed here is therefore
    // stale timer output and must not fall through to ordinary playback.
    if (slot_.state == DurableState::NoStartPending)
        return true;
    const Record* active = occupied_ ? &record_ : alarm_pending_ ? &pending_record_ : nullptr;
    if (!active)
        return false;
    if (source_session != active->alarm.session_id || fault_ ||
        active->outcome != Outcome::Unknown || requested_ != Outcome::Unknown)
        return true;
    if (!started_ || !sentence_ || stopped_ || received_ >= active->alarm.packet_count ||
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
    ClearPreparationIntent();
    if (occupied_ || alarm_pending_)
        Fail(Outcome::Interrupted);
    else if (slot_.state == DurableState::Prepared)
        abandon_pending_ = true;
    if (hooks_.wake)
        hooks_.wake();
}

void Player::Service(const std::string& session, bool negotiated, bool ready, uint32_t press,
                     int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fault_)
        return;

    if (slot_.state == DurableState::Empty) {
        if (preparation_intent_lease_.empty())
            return;
        if (!negotiated || session.empty() || session != preparation_intent_session_ ||
            press != preparation_intent_press_) {
            ClearPreparationIntent();
            return;
        }
        if (!ready)
            return;
        DurableSlot prepared{DurableState::Prepared, preparation_intent_lease_, {}};
        if (!store_.Transition(slot_, prepared)) {
            ClearPreparationIntent();
            fault_ = true;
            return;
        }
        slot_ = std::move(prepared);
        prepared_press_ = press;
        ClearPreparationIntent();
        preparation_uncertain_ = abandon_pending_ = ack_pending_ = false;
        recovery_sent_session_.clear();
        recovery_last_send_us_ = 0;
    }

    if (slot_.state == DurableState::Prepared) {
        if (alarm_pending_) {
            DurableSlot alarm{DurableState::Alarm, pending_record_.alarm.lease_id, pending_record_};
            if (!store_.Transition(slot_, alarm)) {
                fault_ = true;
                packets_.clear();
                return;
            }
            slot_ = std::move(alarm);
            record_ = slot_.record;
            alarm_pending_ = false;
            occupied_ = persisted_ = true;
            ++owner_;
        } else {
            const bool changed_session =
                !recovery_sent_session_.empty() && recovery_sent_session_ != session;
            if (preparation_uncertain_ || abandon_pending_ || press != prepared_press_ ||
                !negotiated || session.empty() || changed_session) {
                DurableSlot no_start{DurableState::NoStartPending, slot_.lease_id, {}};
                if (!store_.Transition(slot_, no_start)) {
                    fault_ = true;
                    return;
                }
                slot_ = std::move(no_start);
                preparation_uncertain_ = abandon_pending_ = ack_pending_ = false;
                recovery_sent_session_.clear();
                recovery_last_send_us_ = 0;
            } else if (ready && (recovery_sent_session_ != session ||
                                 now_us - recovery_last_send_us_ >= 1000000)) {
                const auto proof =
                    RecoveryProofJson(DurableState::Prepared, slot_.lease_id, session);
                if (!proof.empty() && hooks_.send(proof)) {
                    recovery_sent_session_ = session;
                    recovery_last_send_us_ = now_us;
                }
            }
            return;
        }
    }

    if (slot_.state == DurableState::NoStartPending) {
        if (ack_pending_) {
            if (!store_.Erase(slot_)) {
                fault_ = true;
                return;
            }
            slot_ = {};
            ack_pending_ = false;
            recovery_sent_session_.clear();
            recovery_last_send_us_ = 0;
            return;
        }
        if (negotiated && !session.empty() &&
            (recovery_sent_session_ != session || now_us - recovery_last_send_us_ >= 1000000)) {
            const auto proof =
                RecoveryProofJson(DurableState::NoStartPending, slot_.lease_id, session);
            if (!proof.empty() && hooks_.send(proof)) {
                recovery_sent_session_ = session;
                recovery_last_send_us_ = now_us;
            }
        }
        return;
    }

    if (slot_.state != DurableState::Alarm || !occupied_ || !persisted_) {
        fault_ = true;
        return;
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
            DurableSlot terminal_slot{DurableState::Alarm, closed.alarm.lease_id, closed};
            if (!store_.Transition(slot_, terminal_slot)) {
                fault_ = true;
                return;
            }
            record_ = closed;
            slot_ = std::move(terminal_slot);
            if (claimed_)
                hooks_.ended();
        }
    }
    if (record_.outcome == Outcome::Unknown)
        return;
    if (ack_pending_) {
        if (!store_.Erase(slot_) || (claimed_ && !hooks_.release(owner_))) {
            fault_ = true;
            return;
        }
        slot_ = {};
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
