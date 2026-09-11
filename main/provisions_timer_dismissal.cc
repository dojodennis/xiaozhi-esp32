#include "provisions_timer_dismissal.h"

#include <cJSON.h>
#include <algorithm>
#include <cstring>

namespace provisions::timers {
namespace {
bool IsDue(const Timer& timer, int64_t trusted_now_ms) {
    return timer.state == State::Expired || (trusted_now_ms > 0 && timer.deadline_ms <= trusted_now_ms);
}
const char* String(const cJSON* root, const char* key) {
    const auto item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : nullptr;
}
}  // namespace

std::string Dismissals::FormatUuidV4(std::array<uint8_t, 16> bytes) {
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            text.push_back('-');
        text.push_back(digits[bytes[i] >> 4]);
        text.push_back(digits[bytes[i] & 15]);
    }
    return text;
}

std::string Dismissals::FrameJson(const std::string& session, const std::string& timer_id,
                                  uint64_t revision, const std::string& request_id) {
    if (session.empty() || timer_id.empty() || request_id.empty() || revision == 0 ||
        revision > kMaximumRevision)
        return {};
    auto* root = cJSON_CreateObject();
    if (!root)
        return {};
    cJSON_AddStringToObject(root, "session_id", session.c_str());
    cJSON_AddStringToObject(root, "type", "timer");
    cJSON_AddStringToObject(root, "action", "dismiss");
    cJSON_AddStringToObject(root, "timer_id", timer_id.c_str());
    cJSON_AddNumberToObject(root, "revision", static_cast<double>(revision));
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed)
        return {};
    std::string text(printed);
    cJSON_free(printed);
    return text;
}

size_t Dismissals::Dismiss(const Snapshot& snapshot, const std::string& session, bool negotiated,
                           int64_t trusted_now_ms, int64_t now_us, const RequestId& request_id) {
    (void)now_us;
    if (!negotiated || session.empty() || snapshot.session_id != session || !request_id)
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t dismissed = 0;
    for (const auto& timer : snapshot.timers) {
        if (!IsDue(timer, trusted_now_ms) || timer.id.empty() || timer.revision == 0)
            continue;
        const bool already = std::any_of(pending_.begin(), pending_.end(), [&](const Pending& p) {
            return p.timer_id == timer.id && p.revision == timer.revision;
        });
        if (!already) {
            const auto id = request_id();
            if (id.empty())
                continue;
            if (pending_.size() >= kMaximumEntries)
                pending_.erase(pending_.begin());
            pending_.push_back({timer.id, timer.revision, id, 0, 0});
        }
        auto held = std::find_if(suppressed_.begin(), suppressed_.end(),
                                 [&](const Suppressed& s) { return s.timer_id == timer.id; });
        if (held != suppressed_.end()) {
            held->revision = std::max(held->revision, timer.revision);
        } else {
            if (suppressed_.size() >= kMaximumEntries)
                suppressed_.erase(suppressed_.begin());
            suppressed_.push_back({timer.id, timer.revision});
        }
        ++dismissed;
    }
    return dismissed;
}

void Dismissals::Service(const std::string& session, bool negotiated, int64_t now_us,
                         const Send& send) {
    std::vector<std::string> frames;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            const bool due = it->sends == 0 || now_us - it->last_send_us >= kAckTimeoutUs;
            if (!due) {
                ++it;
                continue;
            }
            if (it->sends >= kMaximumSends) {
                // Unanswered after the last retry: the server sweep settles it.
                it = pending_.erase(it);
                continue;
            }
            if (negotiated && !session.empty()) {
                // A reconnect keeps the same request_id under the current session.
                frames.push_back(FrameJson(session, it->timer_id, it->revision, it->request_id));
                ++it->sends;
                it->last_send_us = now_us;
            }
            ++it;
        }
    }
    // Sending happens outside the lock; a failed send already counted as an
    // attempt and is retried after the same ack timeout.
    for (const auto& frame : frames)
        if (!frame.empty() && send)
            send(frame);
}

bool Dismissals::OnAck(const cJSON* root, const std::string& transport_session) {
    const char* type = String(root, "type");
    const char* action = String(root, "action");
    const char* session = String(root, "session_id");
    const char* timer_id = String(root, "timer_id");
    const char* request_id = String(root, "request_id");
    const char* status = String(root, "status");
    if (!type || std::strcmp(type, "timer") != 0 || !action ||
        std::strcmp(action, "dismiss_ack") != 0 || !session || transport_session.empty() ||
        transport_session != session || !timer_id || !request_id || !status ||
        (std::strcmp(status, "settled") != 0 && std::strcmp(status, "unavailable") != 0))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) {
                                      return p.timer_id == timer_id && p.request_id == request_id;
                                  }),
                   pending_.end());
    return true;
}

void Dismissals::Filter(Snapshot& snapshot) {
    if (snapshot.session_id.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = suppressed_.begin(); it != suppressed_.end();) {
        const auto timer = std::find_if(snapshot.timers.begin(), snapshot.timers.end(),
                                        [&](const Timer& t) { return t.id == it->timer_id; });
        if (timer == snapshot.timers.end() || timer->revision > it->revision) {
            it = suppressed_.erase(it);
            continue;
        }
        snapshot.timers.erase(timer);
        ++it;
    }
}

size_t Dismissals::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}
}  // namespace provisions::timers
