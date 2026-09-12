#include "application.h"
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include <sys/time.h>
#include "board.h"
#include "display.h"
#include "websocket_protocol.h"
namespace {
int64_t DictationNow(bool trusted) {
    timeval now{};
    return trusted && gettimeofday(&now, nullptr) == 0 && now.tv_sec > 0
               ? static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000
               : 0;
}
}  // namespace
void Application::ToggleDictationScreen() {
    dictation_screen_.store(!dictation_screen_.load());
    const uint32_t press = provisions_physical_press_.id();
    FenceDictationThrough(press);
    if (manual_listening_requested_.load())
        audio_service_.ReleaseLocalRecordingFence(press);
    xEventGroupSetBits(event_group_, MAIN_EVENT_DICTATION_MODE);
}
void Application::DictationButton() {
    if (!dictation_screen_.load())
        return;
    const uint32_t press = provisions_physical_press_.id();
    FenceDictationThrough(press);
    if (manual_listening_requested_.load())
        audio_service_.ReleaseLocalRecordingFence(press);
    xEventGroupSetBits(event_group_, MAIN_EVENT_DICTATION_CONTROL);
}
void Application::CloseDictationInputOnMain() {
    const uint32_t through = dictation_closed_press_.load();
    std::lock_guard<std::mutex> lock(provisions_recording_control_mutex_);
    auto recorder = std::atomic_load(&provisions_recorder_);
    if (recorder && through)
        recorder->Release(through);
    if (provisions_recording_started_press_ && provisions_recording_started_press_ <= through) {
        audio_service_.StopLocalRecording(provisions_recording_started_press_);
        if (recorder)
            recorder->Release(provisions_recording_started_press_);
        provisions_recording_started_press_ = 0;
    }
    if (through)
        audio_service_.ReconcileLocalRecording(through);
    if (GetDeviceState() == kDeviceStateListening &&
        (!manual_listening_requested_.load() || provisions_physical_press_.id() <= through))
        SetDeviceState(kDeviceStateIdle);
}
void Application::HandleDictationControlOnMain() {
    if (!dictation_screen_.load())
        return;
    auto recorder = std::atomic_load(&provisions_recorder_);
    if (!recorder)
        return;
    const auto r = recorder->DictationRecord();
    using namespace provisions::dictation;
    auto protocol = GetProtocol();
    provisions::VoiceContext context;
    auto* websocket = protocol ? static_cast<WebsocketProtocol*>(protocol.get()) : nullptr;
    const bool negotiated =
        websocket && websocket->DictationNegotiated() && websocket->GetCaptureContext(context);
    if (r.id != provisions::VoiceId{} && !recorder->MatchesConversation(r.conversation_id)) {
        // A fresh Start may retire only a positively empty acknowledged journal.
        // Never strand it by queuing an old-assignment Stop under the new scope.
        if (negotiated && !manual_listening_requested_.load() &&
            provisions_recording_started_press_ == 0 && audio_service_.IsLocalInputIdle() &&
            !timer_player_.Fenced())
            recorder->RequestEmptyDictationReplacement(context.conversation_id);
        return;
    }
    if (r.pending == Action::Start || r.pending == Action::Resume ||
        (r.state == State::Open && r.pending != Action::Stop)) {
        recorder->RequestDictationControl(Action::Stop);
        return;
    }
    if (r.pending != Action::None || recorder->DictationBusy() || recorder->DictationFaulted() ||
        timer_player_.Fenced())
        return;
    if (!negotiated)
        return;
    if (!recorder->MatchesConversation(context.conversation_id))
        return;
    if (r.state == State::Empty || (r.state == State::Reviewed && recorder->PendingCount() == 0))
        recorder->RequestDictationControl(Action::Start);
    else if (r.state == State::Stopped && context.conversation_id == r.conversation_id)
        recorder->RequestDictationControl(Action::Resume);
}
void Application::ServiceDictation() {
    auto recorder = std::atomic_load(&provisions_recorder_);
    if (!recorder)
        return;
    auto protocol = GetProtocol();
    auto* websocket = protocol ? static_cast<WebsocketProtocol*>(protocol.get()) : nullptr;
    provisions::VoiceContext context;
    const bool connected = websocket && websocket->IsAudioChannelOpened();
    const bool negotiated =
        connected && websocket->DictationNegotiated() && websocket->GetCaptureContext(context);
    if (connected) {
        if (!negotiated || (dictation_has_assignment_proof_ &&
                            dictation_assignment_proof_ != context.conversation_id)) {
            dictation_has_assignment_proof_ = false;
            if (dictation_screen_.load() && manual_listening_requested_.load()) {
                FenceDictationThrough(provisions_physical_press_.id());
                audio_service_.ReleaseLocalRecordingFence(provisions_physical_press_.id());
                CloseDictationInputOnMain();
            }
        }
        if (negotiated) {
            if (!dictation_has_assignment_proof_ && dictation_screen_.load())
                FenceDictationThrough(provisions_physical_press_.id());
            dictation_assignment_proof_ = context.conversation_id;
            dictation_has_assignment_proof_ = true;
        }
    }
    const uint32_t authorization = recorder->DictationAuthorization();
    if (authorization != dictation_authorization_seen_) {
        dictation_authorization_seen_ = authorization;
        FenceDictationThrough(provisions_physical_press_.id());
    }
    const auto r = recorder->DictationRecord();
    using namespace provisions::dictation;
    const int64_t now_ms = DictationNow(has_server_time_.load());
    if (r.state == State::Open && now_ms > 0 && now_ms >= r.expires_ms &&
        dictation_screen_.load() && manual_listening_requested_.load()) {
        FenceDictationThrough(provisions_physical_press_.id());
        audio_service_.ReleaseLocalRecordingFence(provisions_physical_press_.id());
        CloseDictationInputOnMain();
    }
    const int64_t tick = esp_timer_get_time();
    if (negotiated && context.conversation_id == r.conversation_id && r.pending == Action::None &&
        (r.state == State::Stopped || (r.state == State::Open && recorder->DictationFaulted())) &&
        tick >= dictation_next_receipt_us_ && recorder->RequestDictationControl(Action::Receipt))
        dictation_next_receipt_us_ = tick + 30000000;
    if (negotiated && context.conversation_id == r.conversation_id &&
        !provisions_network_busy_.load() && !manual_listening_requested_.load() &&
        !provisions_response_pending_.load() && GetDeviceState() == kDeviceStateIdle &&
        (!timer_player_.Fenced() || r.pending == Action::Stop || r.pending == Action::Receipt)) {
        const auto control = ControlJson(r, protocol->session_id());
        const int64_t now = esp_timer_get_time();
        if (!control.empty() &&
            (control != dictation_sent_control_ || now - dictation_last_send_us_ >= 1000000) &&
            websocket->SendDictationControl(control)) {
            dictation_sent_control_ = control;
            dictation_last_send_us_ = now;
        }
    }
    std::string action = "Pending";
    const bool foreign =
        r.id != provisions::VoiceId{} && !recorder->MatchesConversation(r.conversation_id);
    const bool replace_empty = foreign && negotiated && !manual_listening_requested_.load() &&
                               audio_service_.IsLocalInputIdle() &&
                               recorder->CanReplaceEmptyDictation(context.conversation_id);
    if (foreign)
        action = replace_empty ? "Start" : "Recovery";
    else if (r.pending == Action::Start || r.pending == Action::Resume ||
             (r.state == State::Open && r.pending != Action::Stop))
        action = "Stop";
    else if (r.pending == Action::None && r.state == State::Stopped)
        action = "Resume";
    else if (r.pending == Action::None && (r.state == State::Empty || r.state == State::Reviewed))
        action = "Start";
    std::string status;
    if (foreign)
        status = replace_empty ? "Assignment changed - ready to start"
                               : "Previous assignment needs recovery";
    else if (recorder->DictationFaulted())
        status = "Segment pending - needs recovery";
    else if (r.pending != Action::None)
        status = "Control pending";
    else if (r.state == State::Empty)
        status = negotiated ? "Ready to start" : "Connect to start";
    else if (r.state == State::Reviewed)
        status = "Reviewed";
    else if (!now_ms || now_ms >= r.expires_ms)
        status = "Waiting for current session";
    else if (recorder->PendingCount() >= provisions::VoiceOutbox::kSlots)
        status = "Storage full - sync first";
    else if (r.count >= 60)
        status = "60 segments reached - Stop";
    else if (provisions_recording_started_press_ &&
             audio_service_.IsLocalRecordingReady(provisions_recording_started_press_))
        status = "Recording";
    else if (recorder->DictationBusy())
        status = "Saving segment - pending";
    else if (r.state == State::Stopped)
        status = "Stopped";
    else if (!dictation_has_assignment_proof_ || dictation_assignment_proof_ != r.conversation_id)
        status = "Connect to confirm assignment";
    else
        status = "Paused - hold yellow to record";
    unsigned pending = 0;
    for (uint32_t i = 0; i < r.count; ++i)
        if (!r.segments[i].terminal)
            ++pending;
    status += "\n" + std::to_string(r.count) + "/60 segments, " + std::to_string(pending) +
              " pending\nNote / UTC";
    const bool dictation_visible = dictation_screen_.load();
    Board::GetInstance().GetDisplay()->SetDictationScreen(dictation_visible, status, action);
    // Closing the dictation panel only un-hides the face beneath it, which is
    // whatever was last painted - often a stale "Please try again" from an
    // earlier turn. Recompute the resting face on the way out, exactly as a
    // timer dismissal does.
    static bool dictation_was_visible = false;
    if (dictation_was_visible && !dictation_visible && GetDeviceState() == kDeviceStateIdle)
        Board::GetInstance().GetDisplay()->SetStatus(GetProvisionsIdleStatus());
    dictation_was_visible = dictation_visible;
}
#endif
