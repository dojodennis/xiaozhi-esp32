#include "application.h"
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <sys/time.h>
#include "board.h"
#include "display.h"
#include "provisions_voice_wire.h"
#include "settings.h"
#include "websocket_protocol.h"

namespace {
constexpr char kServiceTag[] = "OrbitService";
int64_t ServiceNow() {
    timeval now{};
    return gettimeofday(&now, nullptr) == 0
               ? static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000
               : 0;
}
}  // namespace

bool Application::CanSelectOrbitMode() {
    if (orbit_mode_switch_pending_) {
        auto protocol = GetProtocol();
        if (protocol && protocol->IsAudioChannelOpened() &&
            protocol->session_id() == orbit_mode_switch_session_)
            return false;
        // A failed/replaced connection releases the choice for an explicit retry.
        orbit_mode_switch_pending_ = false;
        orbit_mode_switch_session_.clear();
    }
    auto recorder = std::atomic_load(&provisions_recorder_);
    const auto state = GetDeviceState();
    if (!recorder || IsOrbitWifiSetup() || manual_listening_requested_.load() ||
        provisions_network_busy_.load() || provisions_response_pending_.load() ||
        provisions_recording_saving_.load() || orbit_service_recording_.load() ||
        provisions_timer_ringing_ || timer_player_.Fenced() ||
        (state != kDeviceStateIdle && state != kDeviceStateStarting) ||
        !audio_service_.IsPlaybackIdle() || !audio_service_.IsLocalInputIdle() ||
        recorder->CaptureOpen() || recorder->PendingCount() || recorder->DictationBusy() ||
        recorder->DictationFaulted() || recorder->DictationDiscardPending())
        return false;
    const auto record = recorder->DictationRecord();
    return record.pending == provisions::dictation::Action::None &&
           (record.state == provisions::dictation::State::Empty ||
            record.state == provisions::dictation::State::Reviewed);
}

void Application::SelectOrbitService() {
    if (!CanSelectOrbitMode()) {
        Board::GetInstance().GetDisplay()->ShowNotification("Finish syncing before switching");
        return;
    }
    auto protocol = GetProtocol();
    if (!protocol)
        return;
    if (IsOrbitService()) {
        orbit_service_status_ = "Confirming Chef mode";
        // Keep Service selected until the bound gateway confirms the change.
        if (static_cast<WebsocketProtocol*>(protocol.get())->RequestChefMode()) {
            orbit_mode_switch_pending_ = true;
            orbit_mode_switch_session_ = protocol->session_id();
        } else {
            orbit_service_status_ = "Connect to switch to Chef";
        }
        Board::GetInstance().GetDisplay()->ShowNotification(orbit_service_status_.c_str());
        return;
    }
    LeaveDictationScreenOnMain();
    orbit_service_mode_.store(true);
    Settings("provisions", true).SetInt("dojo_service", 1);
    orbit_service_ready_ = false;
    orbit_service_recording_.store(false);
    orbit_service_status_ = "Connect to approve Service";
    orbit_view_.store(OrbitView::Service);
    dictation_screen_.store(true);
    protocol->CloseAudioChannel();
    provisions_reconnect_wait_ticks_ = 0;
    PaintOrbitService();
}

void Application::OrbitServiceFrame(const std::string& state, const std::string& detail) {
    if (state == "discarded") {
        provisions::VoiceId id;
        auto recorder = std::atomic_load(&provisions_recorder_);
        if (!IsOrbitService() || !recorder || !provisions::ParseVoiceId(detail.c_str(), id) ||
            (recorder->DictationRecord().id != id && !recorder->DictationDiscardPending()))
            return;
        // Fence samples immediately on the authenticated callback. Heavy
        // release/storage work stays on main and the recorder worker.
        orbit_service_recording_.store(false);
        recorder->SetContinuousDictation(false);
        StopListening();
        Schedule([this, recorder, id]() {
            EndLocalRecordingOnMain();
            if (recorder->RequestDiscardedDictation(id)) {
                if (auto protocol = GetProtocol())
                    static_cast<WebsocketProtocol*>(protocol.get())->InterruptStoredRecording();
                orbit_service_status_ = "Discarding at desk";
                PaintOrbitService();
            }
        });
        return;
    }
    Schedule([this, state, detail]() {
        if (!IsOrbitService())
            return;
        if (state == "chef") {
            if (orbit_service_recording_.load())
                return;
            orbit_mode_switch_pending_ = false;
            orbit_mode_switch_session_.clear();
            orbit_service_mode_.store(false);
            Settings("provisions", true).SetInt("dojo_service", 0);
            orbit_service_ready_ = false;
            orbit_service_review_.Reset();
            orbit_service_review_dismissed_ = {};
            orbit_service_code_.clear();
            const bool wifi_setup = IsOrbitWifiSetup();
            if (!wifi_setup) {
                ProvisionsHideOrbitWifiSetup();
                LeaveDictationScreenOnMain();
            }
            if (auto protocol = GetProtocol())
                protocol->CloseAudioChannel();
            provisions_reconnect_wait_ticks_ = 0;
            orbit_menu_index_ = 0;
            // A queued ACK remains authoritative after disconnect, but cannot
            // replace an active Wi-Fi surface. Its finish callback uses this role.
            if (wifi_setup)
                return;
            orbit_view_.store(OrbitView::Menu);
            PaintOrbitView();
            return;
        }
        if (state == "pairing") {
            if (orbit_service_recording_.load())
                StopOrbitServiceCapture();
            orbit_service_ready_ = false;
            orbit_service_review_.Reset();
            orbit_service_code_ = detail;
            orbit_service_status_ = "Approve on the Dojo desk";
            if (orbit_view_.load() == OrbitView::Service && !IsOrbitWifiSetup())
                ProvisionsShowOrbitServicePair("dojo-orbit://pair?code=" + detail);
        } else if (state == "ready" || state == "recovery") {
            orbit_service_recovery_ = state == "recovery";
            orbit_service_ready_ = true;
            orbit_service_code_.clear();
            orbit_service_table_ = detail;
            orbit_service_status_ = "Tap yellow to record";
            if (orbit_view_.load() == OrbitView::Service && !IsOrbitWifiSetup()) {
                ProvisionsHideOrbitWifiSetup();
                dictation_screen_.store(true);
            }
        } else if (state == "approved") {
            orbit_service_code_.clear();
            if (orbit_view_.load() == OrbitView::Service && !IsOrbitWifiSetup())
                ProvisionsHideOrbitWifiSetup();
            orbit_service_status_ = "Loading approved shift";
            if (auto protocol = GetProtocol())
                protocol->CloseAudioChannel();
            provisions_reconnect_wait_ticks_ = 0;
        } else if (state == "expired") {
            orbit_service_code_.clear();
            if (orbit_view_.load() == OrbitView::Service && !IsOrbitWifiSetup())
                ProvisionsHideOrbitWifiSetup();
            orbit_service_ready_ = false;
            orbit_service_status_ = "QR expired. Tap to reconnect";
            orbit_service_review_.Reset();
        }
        PaintOrbitService();
    });
}

void Application::OrbitServicePairing() {
    Schedule([this]() {
        if (!IsOrbitService() || orbit_view_.load() != OrbitView::Service || IsOrbitWifiSetup() ||
            orbit_service_recording_.load())
            return;
        auto protocol = GetProtocol();
        if (protocol && protocol->IsAudioChannelOpened())
            static_cast<WebsocketProtocol*>(protocol.get())->RequestServicePair();
        else {
            provisions_reconnect_wait_ticks_ = 0;
            orbit_service_status_ = "Connect for a fresh pairing QR";
            PaintOrbitService();
        }
    });
}

void Application::OrbitServiceTap() {
    Schedule([this]() {
        if (!IsOrbitService() || orbit_view_.load() != OrbitView::Service || IsOrbitWifiSetup() ||
            timer_player_.Fenced())
            return;
        if (orbit_service_recording_.load()) {
            StopOrbitServiceCapture();
            return;
        }
        using provisions::service::Stage;
        const auto stage = orbit_service_review_.stage();
        if (stage == Stage::Preview || stage == Stage::Target) {
            if (orbit_service_review_.MoreAliasesSelected()) {
                auto cursor = orbit_service_review_.cursor();
                cursor.alias_offset = orbit_service_review_.snapshot().next_alias_offset;
                orbit_service_review_.Navigate(cursor);
                orbit_service_review_send_us_ = 0;
            } else {
                orbit_service_review_.Choose();
            }
            orbit_service_target_page_ = 0;
            PaintOrbitService();
            return;
        }
        if (stage == Stage::Confirm) {
            provisions::VoiceId request;
            esp_fill_random(request.data(), request.size());
            request[6] = (request[6] & 0x0f) | 0x40;
            request[8] = (request[8] & 0x3f) | 0x80;
            orbit_service_review_.Confirm(request);
            orbit_service_review_send_us_ = 0;
            PaintOrbitService();
            return;
        }
        if (stage == Stage::Saving)
            return;
        auto protocol = GetProtocol();
        auto recorder = std::atomic_load(&provisions_recorder_);
        if (!recorder || !orbit_service_ready_ || !protocol || !protocol->IsAudioChannelOpened()) {
            orbit_service_status_ = "Connect and approve your Orbit";
            provisions_reconnect_wait_ticks_ = 0;
            PaintOrbitService();
            return;
        }
        const auto r = recorder->DictationRecord();
        if (orbit_service_recovery_) {
            orbit_service_status_ = "Recovering only. Pair again when synced";
            PaintOrbitService();
            return;
        }
        if (recorder->DictationBusy() || recorder->DictationFaulted() || recorder->PendingCount() ||
            r.pending != provisions::dictation::Action::None) {
            orbit_service_status_ = "Recording kept. Waiting for sync";
            PaintOrbitService();
            return;
        }
        provisions::VoiceContext context;
        auto* websocket = static_cast<WebsocketProtocol*>(protocol.get());
        if (!websocket->DictationNegotiated() || !websocket->GetCaptureContext(context))
            return;
        using namespace provisions::dictation;
        bool started = false;
        if (r.state == State::Empty)
            started = recorder->RequestDictationControl(Action::Start);
        else if (r.conversation_id != context.conversation_id)
            started = recorder->RequestEmptyDictationReplacement(context.conversation_id);
        else if (r.state == State::Reviewed)
            started = recorder->RequestDictationControl(Action::Start);
        else if (r.state == State::Open && r.authorized)
            started = true;
        if (started) {
            orbit_service_review_.Reset();
            orbit_service_review_dismissed_ = {};
            orbit_service_text_page_ = 0;
            orbit_service_target_page_ = 0;
            recorder->SetContinuousDictation(true);
            orbit_service_recording_.store(true);
            orbit_service_status_ = "Preparing recording";
        } else {
            orbit_service_status_ = "Previous recording needs recovery";
        }
        PaintOrbitService();
    });
}

void Application::OrbitServiceReviewFrame(const cJSON* root, const std::string& session) {
    const int64_t received_us = esp_timer_get_time();
    // Parse/copy once on the socket task; no cJSON tree survives the callback.
    provisions::service::Snapshot snapshot;
    provisions::service::Route saved;
    const bool transcript = provisions::service::ParseSnapshot(root, session, snapshot);
    const bool receipt = !transcript && provisions::service::ParseSaved(root, session, saved);
    const auto state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const auto frame_session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const bool current_session =
        cJSON_IsString(frame_session) && session == frame_session->valuestring;
    const bool lost = current_session && cJSON_IsString(state) &&
                      std::string(state->valuestring) == "session_lost";
    const bool refresh = current_session && cJSON_IsString(state) &&
                         std::string(state->valuestring) == "review_required";
    provisions::VoiceId failed_record{}, failed_segment{}, failed_request{}, failed_binding{};
    const auto read_id = [root](const char* key, provisions::VoiceId& id) {
        const auto value = cJSON_GetObjectItemCaseSensitive(root, key);
        return cJSON_IsString(value) && provisions::ParseVoiceId(value->valuestring, id);
    };
    const bool correlated_refresh = refresh && read_id("recording_id", failed_record) &&
                                    read_id("segment_id", failed_segment) &&
                                    read_id("request_id", failed_request) &&
                                    read_id("binding_id", failed_binding);
    if (!transcript && !receipt && !lost && !correlated_refresh)
        return;
    Schedule([this, session, snapshot, saved, transcript, receipt, lost, correlated_refresh,
              received_us, failed_record, failed_segment, failed_request, failed_binding]() {
        const auto protocol = GetProtocol();
        if (!IsOrbitService() || !protocol || protocol->session_id() != session ||
            !protocol->IsAudioChannelOpened())
            return;
        auto* websocket = static_cast<WebsocketProtocol*>(protocol.get());
        provisions::VoiceId binding;
        if (!websocket->GetServiceReviewBinding(binding))
            return;
        if (lost) {
            if (orbit_service_recording_.load())
                StopOrbitServiceCapture();
            orbit_service_ready_ = false;
            orbit_service_review_.Reset();
            orbit_service_status_ = "Shift connection ended. Pair again";
        } else {
            auto recorder = std::atomic_load(&provisions_recorder_);
            if (!recorder)
                return;
            const auto record = recorder->DictationRecord();
            if (record.id == orbit_service_review_dismissed_ ||
                (orbit_service_recording_.load() &&
                 record.state != provisions::dictation::State::Open))
                return;
            if (!orbit_service_review_.matches(record.id, binding, session))
                orbit_service_review_.Reset(record.id, binding, session);
            if (transcript && orbit_service_review_.Accept(snapshot, session)) {
                orbit_service_review_received_us_ = received_us;
                orbit_service_text_page_ = 0;
                orbit_service_target_page_ = 0;
                ESP_LOGI(
                    kServiceTag,
                    "transcript received sequence=%lu chars=%u complete=%d since_last_send_us=%lld",
                    static_cast<unsigned long>(snapshot.cursor.sequence),
                    static_cast<unsigned>(provisions::service::CharacterCount(snapshot.text)),
                    snapshot.complete,
                    orbit_service_review_send_us_ > 0
                        ? static_cast<long long>(esp_timer_get_time() -
                                                 orbit_service_review_send_us_)
                        : -1LL);
            } else if (receipt) {
                if (!orbit_service_review_.Saved(saved, session))
                    return;
            } else if (correlated_refresh) {
                const auto& pending = orbit_service_review_.pending();
                if (pending.binding_id != failed_binding || pending.recording_id != failed_record ||
                    pending.segment_id != failed_segment || pending.request_id != failed_request)
                    return;
                const auto cursor = orbit_service_review_.cursor();
                orbit_service_review_.Reset(record.id, binding, session);
                orbit_service_review_.Navigate(cursor);
                orbit_service_review_send_us_ = 0;
                orbit_service_status_ = "Note changed. Review again";
            }
        }
        PaintOrbitService();
    });
}

void Application::OrbitServiceNavigate(bool forward, bool text_page) {
    Schedule([this, forward, text_page]() {
        if (!IsOrbitService() || orbit_view_.load() != OrbitView::Service || IsOrbitWifiSetup() ||
            orbit_service_recording_.load() || timer_player_.Fenced())
            return;
        using provisions::service::Stage;
        const auto stage = orbit_service_review_.stage();
        if (stage == Stage::Saving || stage == Stage::None)
            return;
        const auto snapshot = orbit_service_review_.snapshot();
        if (text_page &&
            (stage == Stage::Target || stage == Stage::Confirm || stage == Stage::Saving)) {
            uint32_t pages;
            provisions::service::PreviewPage(orbit_service_review_.TargetLabel(),
                                             orbit_service_target_page_, pages);
            if (forward && orbit_service_target_page_ + 1 < pages)
                ++orbit_service_target_page_;
            else if (!forward && orbit_service_target_page_)
                --orbit_service_target_page_;
        } else if (text_page && (stage == Stage::Preview || stage == Stage::Saved)) {
            uint32_t pages;
            provisions::service::PreviewPage(snapshot.text, orbit_service_text_page_, pages);
            if (forward && orbit_service_text_page_ + 1 < pages)
                ++orbit_service_text_page_;
            else if (!forward && orbit_service_text_page_)
                --orbit_service_text_page_;
            else if (forward && snapshot.next_text_offset >= 0) {
                auto cursor = snapshot.cursor;
                cursor.text_offset = snapshot.next_text_offset;
                orbit_service_review_.Navigate(cursor);
                orbit_service_review_send_us_ = 0;
                orbit_service_text_page_ = 0;
            } else if (!forward && snapshot.cursor.text_offset) {
                auto cursor = snapshot.cursor;
                cursor.text_offset = cursor.text_offset > 500 ? cursor.text_offset - 500 : 0;
                orbit_service_review_.Navigate(cursor);
                orbit_service_review_send_us_ = 0;
                orbit_service_text_page_ = 0;
            }
        } else if (stage == Stage::Target && forward) {
            orbit_service_review_.NextTarget();
            orbit_service_target_page_ = 0;
        } else if (stage == Stage::Confirm || stage == Stage::Target) {
            orbit_service_review_.CancelChoice();
        } else if (snapshot.next_sequence >= 0 && forward) {
            auto cursor = snapshot.cursor;
            cursor.sequence = snapshot.next_sequence;
            cursor.text_offset = 0;
            cursor.alias_offset = 0;
            orbit_service_review_.Navigate(cursor);
            orbit_service_review_send_us_ = 0;
            orbit_service_text_page_ = 0;
        } else {
            orbit_service_review_dismissed_ = snapshot.recording_id;
            orbit_service_review_.Reset();
            orbit_service_status_ = "Notes kept at desk. Yellow records";
        }
        PaintOrbitService();
    });
}

void Application::StopOrbitServiceCapture() {
    if (!IsOrbitService())
        return;
    orbit_service_recording_.store(false);
    if (auto recorder = std::atomic_load(&provisions_recorder_))
        recorder->SetContinuousDictation(false);
    StopListening();  // Atomic sample fence applies before the main-task work.
    Schedule([this]() {
        EndLocalRecordingOnMain();
        if (auto recorder = std::atomic_load(&provisions_recorder_)) {
            auto r = recorder->DictationRecord();
            if (r.state == provisions::dictation::State::Open ||
                r.pending == provisions::dictation::Action::Start)
                recorder->RequestDictationControl(provisions::dictation::Action::Stop);
        }
        orbit_service_status_ =
            orbit_service_ready_ ? "Stopped. Sending for review" : "Shift ended. Recording kept";
        PaintOrbitService();
    });
}

void Application::TickOrbitService() {
    if (!IsOrbitService())
        return;
    auto recorder = std::atomic_load(&provisions_recorder_);
    if (!recorder)
        return;
    const auto r = recorder->DictationRecord();
    if (recorder->DictationDiscardPending()) {
        orbit_service_status_ = recorder->DictationFaulted()
                                    ? "Discard approved. Storage retry pending"
                                    : "Discarding at desk";
        PaintOrbitService();
        return;
    }
    const auto protocol = GetProtocol();
    const bool connected = protocol && protocol->IsAudioChannelOpened();
    if (!connected && orbit_service_review_.stage() != provisions::service::Stage::None) {
        orbit_service_review_.Reset();
        orbit_service_status_ = "Connection lost. Recording kept";
    }
    if (connected && orbit_service_ready_ && !orbit_service_recovery_) {
        auto* websocket = static_cast<WebsocketProtocol*>(protocol.get());
        provisions::VoiceId binding;
        if (r.id != provisions::VoiceId{} && r.id != orbit_service_review_dismissed_ &&
            websocket->GetServiceReviewBinding(binding) &&
            (!orbit_service_recording_.load() || r.state == provisions::dictation::State::Open)) {
            if (!orbit_service_review_.matches(r.id, binding, protocol->session_id()))
                orbit_service_review_.Reset(r.id, binding, protocol->session_id());
            const int64_t now = esp_timer_get_time();
            using provisions::service::Stage;
            const auto stage = orbit_service_review_.stage();
            const bool waiting =
                stage == Stage::None ||
                (stage == Stage::Preview && !orbit_service_review_.snapshot().complete);
            if (!provisions_network_busy_.load() &&
                (orbit_service_review_send_us_ == 0 ||
                 now - orbit_service_review_send_us_ >= 1000000)) {
                std::string message;
                if (stage == Stage::Saving)
                    message = provisions::service::RouteJson(protocol->session_id(),
                                                             orbit_service_review_.pending());
                else if (waiting && !orbit_service_recording_.load())
                    message = provisions::service::ReviewJson(protocol->session_id(), r.id,
                                                              orbit_service_review_.cursor());
                if (!message.empty() && websocket->SendServiceReview(message))
                    orbit_service_review_send_us_ = now;
            }
        }
    }
    if (!orbit_service_recording_.load() && r.state == provisions::dictation::State::Open &&
        r.pending == provisions::dictation::Action::None &&
        (orbit_service_recovery_ || !r.authorized)) {
        recorder->RequestDictationControl(provisions::dictation::Action::Stop);
        orbit_service_status_ = "Recovering saved recording";
    }
    if (orbit_service_recording_.load()) {
        if (timer_player_.Fenced() || recorder->DictationFaulted() ||
            (r.expires_ms > 0 && has_server_time_ && ServiceNow() >= r.expires_ms) ||
            recorder->PendingCount() >= provisions::VoiceOutbox::kSlots) {
            StopOrbitServiceCapture();
            orbit_service_status_ = "Stopped. Recording kept for sync";
            return;
        }
        if (recorder->DictationCapped(provisions_physical_press_.id())) {
            StopOrbitServiceCapture();
            orbit_service_status_ = "Stopped. Recording kept for sync";
            return;
        }
        if (!manual_listening_requested_.load() && r.state == provisions::dictation::State::Open &&
            r.authorized && r.pending == provisions::dictation::Action::None &&
            !recorder->DictationBusy() && audio_service_.IsLocalInputIdle())
            StartListening();
        if (connected && !provisions_network_busy_.load())
            recorder->RequestReplay();
    } else if (orbit_service_ready_ && r.state == provisions::dictation::State::Reviewed &&
               !recorder->PendingCount()) {
        orbit_service_status_ = "Sent to Dojo. Review at the desk";
    }
    if (orbit_view_.load() == OrbitView::Service)
        PaintOrbitService();
}

void Application::PaintOrbitService() {
    if (!IsOrbitService() || orbit_view_.load() != OrbitView::Service || IsOrbitWifiSetup() ||
        !orbit_service_code_.empty())
        return;
    std::string status = orbit_service_status_;
    std::string body, help;
    uint32_t displayed_pages = 0;
    ProvisionsServicePhase phase = ProvisionsServicePhase::Ready;
    const auto protocol = GetProtocol();
    const bool connected = protocol && protocol->IsAudioChannelOpened();
    if (orbit_service_recording_.load()) {
        const bool recording =
            manual_listening_requested_.load() &&
            audio_service_.IsLocalRecordingReady(provisions_physical_press_.id());
        auto recorder = std::atomic_load(&provisions_recorder_);
        const bool saving =
            recorder && recorder->DictationRecord().state == provisions::dictation::State::Open &&
            manual_listening_requested_.load();
        status = recording ? "Recording" : saving ? "Saving segment" : "Preparing microphone";
        phase = recording ? ProvisionsServicePhase::Recording : ProvisionsServicePhase::Preparing;
        help = "yellow stops";
        if (!connected)
            status += " · OFFLINE";
    } else if (!orbit_service_ready_ || !connected || orbit_service_recovery_) {
        phase = ProvisionsServicePhase::Lost;
        help = "hold blue to pair · recording kept";
        if (orbit_service_recovery_)
            status = "Sync recovery. Pair again afterward";
        else if (!connected)
            status = "Connection lost. Recording kept";
    } else {
        using provisions::service::Stage;
        const auto stage = orbit_service_review_.stage();
        const auto& snapshot = orbit_service_review_.snapshot();
        if (stage == Stage::Target || stage == Stage::Confirm) {
            phase = ProvisionsServicePhase::Received;
            status = stage == Stage::Confirm ? "Confirm this note" : "Choose destination";
            body = provisions::service::PreviewPage(orbit_service_review_.TargetLabel(),
                                                    orbit_service_target_page_, displayed_pages);
            help = stage == Stage::Confirm ? "yellow confirms · blue cancels"
                                           : "blue changes · yellow chooses";
            if (displayed_pages > 1)
                help = "swipe up/down to read destination\n" + help;
            if (!orbit_service_review_.GuestAllowed())
                help += "\nGuest notes need desk review";
        } else if (stage == Stage::Saving) {
            phase = ProvisionsServicePhase::Saving;
            status = "Saving note to Dojo";
            body = provisions::service::PreviewPage(orbit_service_review_.TargetLabel(),
                                                    orbit_service_target_page_, displayed_pages);
            help = "Waiting for saved receipt";
        } else if (stage == Stage::Preview || stage == Stage::Saved) {
            phase = stage == Stage::Saved ? ProvisionsServicePhase::Saved
                    : snapshot.complete   ? ProvisionsServicePhase::Received
                                          : ProvisionsServicePhase::Processing;
            body = provisions::service::PreviewPage(snapshot.text, orbit_service_text_page_,
                                                    displayed_pages);
            if (body.empty())
                body = "No transcript yet";
            status = stage == Stage::Saved ? "Note saved"
                     : snapshot.complete   ? "Transcript received"
                                           : "Transcribing";
            status += " · part " + std::to_string(snapshot.cursor.sequence + 1);
            help = "swipe up/down to read";
            if (snapshot.complete)
                help += stage == Stage::Saved ? "\nyellow records · blue next note"
                        : orbit_service_review_.text_reviewed()
                            ? "\nyellow chooses · blue next note"
                            : "\nRead all pages before choosing";
            else
                help += "\nWaiting for complete recording";
        } else {
            auto recorder = std::atomic_load(&provisions_recorder_);
            if (recorder &&
                recorder->DictationRecord().state == provisions::dictation::State::Stopped) {
                phase = ProvisionsServicePhase::Processing;
                status = "Processing transcript";
                help = "Recording kept. Waiting for Dojo";
            } else {
                help = "yellow records · hold blue to pair";
            }
        }
    }
    if (ProvisionsShowOrbitService(phase, status, body, help)) {
        using provisions::service::Stage;
        if (orbit_service_review_.stage() == Stage::Preview) {
            orbit_service_review_.MarkTextPage(orbit_service_text_page_, displayed_pages);
            if (orbit_service_review_received_us_ > 0) {
                ESP_LOGI(kServiceTag, "transcript receipt_to_paint_us=%lld",
                         static_cast<long long>(esp_timer_get_time() -
                                                orbit_service_review_received_us_));
                orbit_service_review_received_us_ = 0;
            }
        } else if (orbit_service_review_.stage() == Stage::Target)
            orbit_service_review_.MarkTargetPage(orbit_service_target_page_, displayed_pages);
    }
}
#endif
