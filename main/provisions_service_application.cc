#include "application.h"
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include <sys/time.h>
#include "board.h"
#include "display.h"
#include "settings.h"
#include "websocket_protocol.h"

namespace {
int64_t ServiceNow() {
    timeval now{};
    return gettimeofday(&now, nullptr) == 0
               ? static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000
               : 0;
}
}  // namespace

void Application::SelectOrbitService() {
    auto recorder = std::atomic_load(&provisions_recorder_);
    if (!recorder || IsOrbitWifiSetup() || manual_listening_requested_.load() ||
        provisions_network_busy_.load() || recorder->CaptureOpen() || recorder->PendingCount() ||
        recorder->DictationBusy() || recorder->DictationFaulted()) {
        Board::GetInstance().GetDisplay()->ShowNotification("Finish syncing before switching");
        return;
    }
    const auto record = recorder->DictationRecord();
    if (record.pending != provisions::dictation::Action::None ||
        (record.count && record.state != provisions::dictation::State::Reviewed)) {
        Board::GetInstance().GetDisplay()->ShowNotification("Finish the recording first");
        return;
    }
    auto protocol = GetProtocol();
    if (!protocol)
        return;
    if (IsOrbitService()) {
        orbit_service_status_ = "Confirming Chef mode";
        // Keep Service selected until the bound gateway confirms the change.
        if (!static_cast<WebsocketProtocol*>(protocol.get())->RequestChefMode())
            orbit_service_status_ = "Connect to switch to Chef";
        PaintOrbitService();
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
    Schedule([this, state, detail]() {
        if (!IsOrbitService())
            return;
        if (state == "chef") {
            if (orbit_service_recording_.load())
                return;
            orbit_service_mode_.store(false);
            Settings("provisions", true).SetInt("dojo_service", 0);
            orbit_service_ready_ = false;
            orbit_service_code_.clear();
            ProvisionsHideOrbitWifiSetup();
            LeaveDictationScreenOnMain();
            if (auto protocol = GetProtocol())
                protocol->CloseAudioChannel();
            provisions_reconnect_wait_ticks_ = 0;
            OpenOrbitShoppingOnMain();
            return;
        }
        if (state == "pairing") {
            StopOrbitServiceCapture();
            orbit_service_ready_ = false;
            orbit_service_code_ = detail;
            orbit_service_status_ = "Approve on the Dojo desk";
            ProvisionsShowOrbitServicePair("dojo-orbit://pair?code=" + detail);
        } else if (state == "ready" || state == "recovery") {
            orbit_service_recovery_ = state == "recovery";
            orbit_service_ready_ = true;
            orbit_service_code_.clear();
            orbit_service_table_ = detail;
            orbit_service_status_ = "Tap yellow to record";
            ProvisionsHideOrbitWifiSetup();
            orbit_view_.store(OrbitView::Service);
            dictation_screen_.store(true);
        } else if (state == "approved") {
            orbit_service_code_.clear();
            ProvisionsHideOrbitWifiSetup();
            orbit_service_status_ = "Loading approved table";
            if (auto protocol = GetProtocol())
                protocol->CloseAudioChannel();
            provisions_reconnect_wait_ticks_ = 0;
        } else if (state == "expired") {
            orbit_service_code_.clear();
            ProvisionsHideOrbitWifiSetup();
            orbit_service_ready_ = false;
            orbit_service_status_ = "QR expired. Tap to reconnect";
        }
        PaintOrbitService();
    });
}

void Application::OrbitServicePairing() {
    Schedule([this]() {
        if (!IsOrbitService() || orbit_service_recording_.load())
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
        if (!IsOrbitService() || orbit_view_.load() == OrbitView::Menu)
            return;
        if (orbit_service_recording_.load()) {
            StopOrbitServiceCapture();
            return;
        }
        auto protocol = GetProtocol();
        auto recorder = std::atomic_load(&provisions_recorder_);
        if (!recorder || !orbit_service_ready_ || !protocol || !protocol->IsAudioChannelOpened()) {
            orbit_service_status_ = "Connect and approve this table";
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
            recorder->SetContinuousDictation(true);
            orbit_service_recording_.store(true);
            orbit_service_status_ = "Preparing recording";
        } else {
            orbit_service_status_ = "Previous recording needs recovery";
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
        orbit_service_status_ = "Stopped. Sending for review";
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
    const auto protocol = GetProtocol();
    const bool connected = protocol && protocol->IsAudioChannelOpened();
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
    } else if (r.state == provisions::dictation::State::Reviewed && !recorder->PendingCount()) {
        orbit_service_status_ = "Sent to Dojo. Review at the desk";
    }
    if (orbit_view_.load() == OrbitView::Service)
        PaintOrbitService();
}

void Application::PaintOrbitService() {
    if (!IsOrbitService() || !orbit_service_code_.empty())
        return;
    std::string status = orbit_service_status_;
    if (orbit_service_recording_.load()) {
        const auto protocol = GetProtocol();
        const bool recording =
            manual_listening_requested_.load() &&
            audio_service_.IsLocalRecordingReady(provisions_physical_press_.id());
        status = recording ? "RECORDING" : "Saving segment";
        if (!protocol || !protocol->IsAudioChannelOpened())
            status += " · OFFLINE";
    }
    ProvisionsShowShoppingFocus("Dojo · Table " + orbit_service_table_, status,
                                orbit_service_recording_.load()
                                    ? "tap yellow to finish"
                                    : "yellow records · hold blue to pair",
                                "Service");
}
#endif
