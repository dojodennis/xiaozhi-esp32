#include "application.h"
#if CONFIG_PROVISIONS_LOCAL_CAPTURE
#include <algorithm>
#include <esp_log.h>
#include <sys/time.h>
#include "assets/lang_config.h"
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
std::string DictationHeardFace(const std::string& heard, bool ok) {
    std::string status;
    status.reserve(heard.size() + 12);
    for (char character : heard)
        status += character == '|' ? '\n' : character;
    status += '\n';
    status += ok ? "RECORDED" : "TRY AGAIN";
    return status;
}
uint8_t g_shopping_focus = 0;
int64_t g_shopping_focus_since_us = 0;
uint8_t g_shopping_appended = 0;
uint8_t g_shopping_batch = 0;
// Newest items hidden below the scrolled page; 0 means live (newest showing).
size_t g_shopping_scroll = 0;
bool g_shopping_read_face = false;
uint8_t g_notes_focus = 0;

std::string LowerAscii(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
    return text;
}
int64_t g_shopping_scroll_at_us = 0;
constexpr size_t kShoppingPage = 5;
constexpr size_t kShoppingPageStep = kShoppingPage - 1;  // one line of overlap
constexpr int64_t kShoppingScrollIdleUs = 8 * 1000000;
}  // namespace
void Application::ToggleDictationScreen() {
    const bool next = !dictation_screen_.load();
    dictation_screen_.store(next);
    if (!next) {
        dictation_heard_.clear();
        dictation_heard_incoming_.clear();
        dictation_heard_ok_ = true;
        dictation_review_pending_ = false;
        dictation_review_since_us_ = 0;
        dictation_heard_at_us_ = 0;
        dictation_readback_.store(false);
        dictation_heard_voice_done_ = false;
        if (GetDeviceState() == kDeviceStateSpeaking)
            AbortSpeaking(kAbortReasonNone);
    }
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
    if (!dictation_heard_.empty()) {
        dictation_heard_.clear();
        dictation_heard_incoming_.clear();
        dictation_heard_ok_ = true;
        dictation_review_pending_ = false;
        dictation_review_since_us_ = 0;
        dictation_heard_at_us_ = 0;
        dictation_readback_.store(false);
        dictation_heard_voice_done_ = false;
        if (GetDeviceState() == kDeviceStateSpeaking)
            AbortSpeaking(kAbortReasonNone);
        ServiceDictation();
        return;
    }
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
    auto start_blocked = [&] {
        if (r.state == State::Reviewed && recorder->PendingCount() != 0)
            return true;
        if (r.state == State::Reviewed || r.state == State::Empty) {
            for (uint32_t i = 0; i < r.count; ++i) {
                if (!r.segments[i].terminal)
                    return true;
            }
        }
        return false;
    };
    if (r.id != provisions::VoiceId{} && !recorder->MatchesConversation(r.conversation_id)) {
        // A fresh Start may retire only a positively empty acknowledged journal.
        // Never strand it by queuing an old-assignment Stop under the new scope.
        if (negotiated && !manual_listening_requested_.load() &&
            provisions_recording_started_press_ == 0 && audio_service_.IsLocalInputIdle() &&
            !timer_player_.Fenced())
            recorder->RequestEmptyDictationReplacement(context.conversation_id);
        else
            PlaySound(Lang::Sounds::OGG_POPUP);
        return;
    }
    if (r.pending == Action::Start || r.pending == Action::Resume)
        return;
    if (r.state == State::Open && r.pending != Action::Stop) {
        recorder->RequestDictationControl(Action::Stop);
        return;
    }
    if (r.pending != Action::None || recorder->DictationBusy() || recorder->DictationFaulted() ||
        timer_player_.Fenced()) {
        PlaySound(Lang::Sounds::OGG_POPUP);
        return;
    }
    if (!negotiated || !recorder->MatchesConversation(context.conversation_id)) {
        PlaySound(Lang::Sounds::OGG_POPUP);
        return;
    }
    if (r.state == State::Empty || (r.state == State::Reviewed && !start_blocked()))
        recorder->RequestDictationControl(Action::Start);
    else if (r.state == State::Stopped && context.conversation_id == r.conversation_id)
        recorder->RequestDictationControl(Action::Resume);
    else
        PlaySound(Lang::Sounds::OGG_POPUP);
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
    // Orbit Lite negotiates no dictation and carries no assignment: it neither
    // confirms nor revokes the RAM assignment proof, which stays for the next
    // full-gateway session.
    const bool lite = connected && websocket->IsLiteMode();
    if (connected && !lite) {
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
    auto start_blocked = [&] {
        if (r.state == State::Reviewed && recorder->PendingCount() != 0)
            return true;
        if (r.state == State::Reviewed || r.state == State::Empty) {
            for (uint32_t i = 0; i < r.count; ++i) {
                if (!r.segments[i].terminal)
                    return true;
            }
        }
        return false;
    };
    const int64_t now_ms = DictationNow(has_server_time_.load());
    if (r.state == State::Open && now_ms > 0 && now_ms >= r.expires_ms &&
        dictation_screen_.load() && manual_listening_requested_.load()) {
        FenceDictationThrough(provisions_physical_press_.id());
        audio_service_.ReleaseLocalRecordingFence(provisions_physical_press_.id());
        CloseDictationInputOnMain();
    }
    const int64_t tick = esp_timer_get_time();
    const bool resume_stopped = dictation_screen_.load() && negotiated && !lite &&
                                context.conversation_id == r.conversation_id &&
                                r.pending == Action::None && r.state == State::Stopped &&
                                !recorder->DictationBusy() && !recorder->DictationFaulted() &&
                                !timer_player_.Fenced();
    const bool start_empty = dictation_screen_.load() && negotiated && !lite &&
                             context.conversation_id == r.conversation_id &&
                             r.pending == Action::None && r.state == State::Empty &&
                             !start_blocked() && !recorder->DictationBusy() &&
                             !recorder->DictationFaulted() && !timer_player_.Fenced();
    if (resume_stopped)
        recorder->RequestDictationControl(Action::Resume);
    else if (start_empty)
        recorder->RequestDictationControl(Action::Start);
    else if (negotiated && context.conversation_id == r.conversation_id &&
             r.pending == Action::None &&
             (r.state == State::Stopped ||
              (r.state == State::Open && recorder->DictationFaulted())) &&
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
        action = start_blocked() ? "Wait" : "Start";
    std::string status;
    if (foreign)
        status = replace_empty ? "Assignment changed - ready to start"
                               : "Previous assignment needs recovery";
    else if (recorder->DictationFaulted())
        status = "Segment pending - needs recovery";
    else if (lite)
        status = "Dictation needs the full gateway";
    else if (r.pending != Action::None)
        status = (r.pending == Action::Start || r.pending == Action::Resume)
                     ? "Hold yellow to record"
                     : "Control pending";
    else if (r.state == State::Empty)
        status = negotiated ? "Hold yellow to record" : "Connect to start";
    else if (r.state == State::Reviewed)
        status = start_blocked() ? "Clip still sending - wait" : "Reviewed";
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
        status = "Hold yellow to record";
    else if (!dictation_has_assignment_proof_ || dictation_assignment_proof_ != r.conversation_id)
        status = "Connect to confirm assignment";
    else
        status = "Hold yellow to record";
    const bool recording = provisions_recording_started_press_ &&
                           audio_service_.IsLocalRecordingReady(provisions_recording_started_press_);
    static bool was_recording = false;
    if (recording && !was_recording) {
        dictation_heard_.clear();
        dictation_heard_incoming_.clear();
        dictation_review_pending_ = false;
        dictation_review_since_us_ = 0;
        dictation_heard_at_us_ = 0;
        dictation_readback_.store(false);
        dictation_heard_voice_done_ = false;
    }
    was_recording = recording;
    if (!recording && !dictation_heard_incoming_.empty()) {
        dictation_heard_ = std::move(dictation_heard_incoming_);
        dictation_heard_incoming_.clear();
        dictation_heard_ok_ = dictation_heard_incoming_ok_;
        dictation_review_pending_ = true;
        dictation_heard_at_us_ = esp_timer_get_time();
    }
    if (!dictation_heard_.empty()) {
        status = DictationHeardFace(dictation_heard_, dictation_heard_ok_);
    } else if (dictation_review_pending_ && !recording) {
        status = "Saving";
    }
    if (IsOrbitWifiSetup())
        return;
    if (orbit_view_.load() == OrbitView::Menu || orbit_view_.load() == OrbitView::Notes)
        return;
    if (orbit_view_.load() == OrbitView::Shopping) {
        if (g_shopping_scroll > 0 &&
            esp_timer_get_time() - g_shopping_scroll_at_us >= kShoppingScrollIdleUs) {
            g_shopping_scroll = 0;
            PaintOrbitView();
            return;
        }
        if (g_shopping_scroll == 0 && g_shopping_batch == 0 &&
            GetDeviceState() == kDeviceStateSpeaking &&
            shopping_list_items_.size() > 1) {
            const int64_t now = esp_timer_get_time();
            if (g_shopping_focus_since_us == 0)
                g_shopping_focus_since_us = now;
            if (now - g_shopping_focus_since_us >= 1100000 &&
                g_shopping_focus + 1 < shopping_list_items_.size()) {
                g_shopping_focus = static_cast<uint8_t>(g_shopping_focus + 1);
                g_shopping_focus_since_us = now;
                PaintOrbitView();
            }
        }
        return;
    }
    if (ProvisionsTimerFaceShowing())
        return;
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

void Application::HandleOrbitMenuBlue() {
    Schedule([this]() { HandleOrbitMenuBlueOnMain(); });
}

bool Application::IsOrbitShoppingFace() const {
    return orbit_view_.load() == OrbitView::Shopping;
}

bool Application::IsOrbitNotesFace() const {
    return orbit_view_.load() == OrbitView::Notes;
}

bool ProvisionsListenNotes() {
    return Application::GetInstance().IsOrbitNotesFace();
}

bool Application::IsOrbitMenuFace() const {
    return orbit_view_.load() == OrbitView::Menu;
}

bool ProvisionsListenShopping() {
    return Application::GetInstance().IsOrbitShoppingFace();
}

void Application::HandleShoppingSwipe(bool down) {
    Schedule([this, down]() {
        if (orbit_view_.load() == OrbitView::Notes) {
            if (notes_items_.empty())
                return;
            const size_t last = notes_items_.size() - 1;
            size_t focus = g_notes_focus;
            if (down)
                focus = focus > 0 ? focus - 1 : 0;
            else
                focus = std::min(focus + 1, last);
            if (focus == g_notes_focus)
                return;
            g_notes_focus = static_cast<uint8_t>(std::min(focus, size_t{255}));
            PaintOrbitView();
            return;
        }
        if (orbit_view_.load() != OrbitView::Shopping)
            return;
        const size_t count = shopping_list_items_.size();
        const size_t max_scroll = count > kShoppingPage ? count - kShoppingPage : 0;
        size_t scroll = g_shopping_scroll;
        if (down)
            scroll = std::min(scroll + kShoppingPageStep, max_scroll);
        else
            scroll = scroll > kShoppingPageStep ? scroll - kShoppingPageStep : 0;
        g_shopping_scroll_at_us = esp_timer_get_time();
        ESP_LOGI("Shopping", "swipe %s: items %u scroll %u -> %u", down ? "down" : "up",
                 static_cast<unsigned>(count), static_cast<unsigned>(g_shopping_scroll),
                 static_cast<unsigned>(scroll));
        if (scroll == g_shopping_scroll)
            return;
        g_shopping_scroll = scroll;
        PaintOrbitView();
    });
}

bool Application::ConfirmOrbitMenu() {
    if (IsOrbitWifiSetup())
        return true;
    if (orbit_view_.load() != OrbitView::Menu)
        return false;
    const bool timers = orbit_menu_index_ == 1;
    Schedule([this]() { ConfirmOrbitMenuOnMain(); });
    return timers;
}

// Entry is deliberately wired only after Dennis chooses the menu placement.
void Application::StartOrbitWifiSetup() {
    Schedule([this]() {
        if (IsOrbitWifiSetup() || GetDeviceState() != kDeviceStateIdle ||
            manual_listening_requested_.load() || provisions_network_busy_.load() ||
            provisions_response_pending_.load() || provisions_recording_saving_.load() ||
            !audio_service_.IsPlaybackIdle() || timer_player_.Fenced())
            return;
        LeaveDictationScreenOnMain();
        orbit_wifi_setup_.store(true);
        SetDeviceState(kDeviceStateWifiConfiguring);
        ProvisionsShowOrbitWifiSetup("");
        auto finish = [this](bool saved) {
            Schedule([this, saved]() {
                orbit_wifi_setup_.store(false);
                ProvisionsHideOrbitWifiSetup();
                SetDeviceState(kDeviceStateIdle);
                orbit_view_.store(OrbitView::Menu);
                PaintOrbitView();
                Board::GetInstance().GetDisplay()->ShowNotification(
                    saved ? "wi-fi saved — reconnecting" : "setup closed — retrying saved wi-fi");
            });
        };
        if (!Board::GetInstance().StartWifiSetup(
                [this](std::string payload) {
                    Schedule([this, payload = std::move(payload)]() {
                        if (IsOrbitWifiSetup())
                            ProvisionsShowOrbitWifiSetup(payload);
                    });
                },
                finish))
            finish(false);
    });
}
void Application::CancelOrbitWifiSetup() {
    if (IsOrbitWifiSetup())
        Board::GetInstance().CancelWifiSetup();
}

void Application::HandleOrbitMenuBlueOnMain() {
    if (IsOrbitWifiSetup()) {
        CancelOrbitWifiSetup();
        return;
    }
    if (orbit_view_.load() == OrbitView::Menu) {
        orbit_menu_index_ = static_cast<uint8_t>((orbit_menu_index_ + 1) % 3);
        PaintOrbitView();
        return;
    }
    LeaveDictationScreenOnMain();
    orbit_view_.store(OrbitView::Menu);
    PaintOrbitView();
}

void Application::LeaveDictationScreenOnMain() {
    if (!dictation_screen_.load())
        return;
    dictation_screen_.store(false);
    FenceDictationThrough(provisions_physical_press_.id());
    if (manual_listening_requested_.load())
        audio_service_.ReleaseLocalRecordingFence(provisions_physical_press_.id());
}

// The menu's two destinations, also reached by a sideways swipe.
void Application::OpenOrbitShoppingOnMain() {
    LeaveDictationScreenOnMain();
    g_shopping_scroll = 0;  // Arrive on the newest items, not a stale page.
    orbit_view_.store(OrbitView::Shopping);
    PaintOrbitView();
}

void Application::OpenOrbitTimersOnMain() {
    orbit_view_.store(OrbitView::Home);
    if (!dictation_screen_.load())
        Board::GetInstance().GetDisplay()->SetDictationScreen(false, "", "");
    ProvisionsShowTimerFace();
}

void Application::OpenOrbitNotesOnMain() {
    LeaveDictationScreenOnMain();
    orbit_view_.store(OrbitView::Notes);
    PaintOrbitView();
}

void Application::ConfirmOrbitMenuOnMain() {
    if (orbit_view_.load() != OrbitView::Menu)
        return;
    if (orbit_menu_index_ == 0)
        OpenOrbitShoppingOnMain();
    else if (orbit_menu_index_ == 2)
        OpenOrbitNotesOnMain();
    else
        OpenOrbitTimersOnMain();
}

void Application::HandleOrbitFaceSwipe(bool right) {
    Schedule([this, right]() {
        if (IsOrbitWifiSetup())
            return;
        // The menu is its own pager: a sideways swipe flips the logo
        // (list <-> timer) and stays there until yellow confirms.
        if (orbit_view_.load() == OrbitView::Menu) {
            orbit_menu_index_ = static_cast<uint8_t>(
                (orbit_menu_index_ + (right ? 1 : 2)) % 3);
            PaintOrbitView();
            return;
        }
        uint8_t page = 1;
        if (orbit_view_.load() == OrbitView::Shopping)
            page = 0;
        else if (orbit_view_.load() == OrbitView::Notes)
            page = 2;
        orbit_menu_index_ = static_cast<uint8_t>((page + (right ? 1 : 2)) % 3);
        if (orbit_menu_index_ == 0)
            OpenOrbitShoppingOnMain();
        else if (orbit_menu_index_ == 2)
            OpenOrbitNotesOnMain();
        else
            OpenOrbitTimersOnMain();
    });
}

namespace {
bool LooksLikeQuantity(const std::string& part) {
    if (part.empty())
        return false;
    size_t index = 0;
    if (part[0] < '0' || part[0] > '9') {
        std::string lower = part;
        for (char& character : lower) {
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character + ('a' - 'A'));
        }
        static const char* words[] = {"a", "an", "one", "two", "three", "four", "five",
                                      "six", "seven", "eight", "nine", "ten", "eleven",
                                      "twelve", "thirteen", "fourteen", "fifteen"};
        for (const char* word : words) {
            if (lower == word)
                return true;
        }
        return false;
    }
    while (index < part.size() &&
           ((part[index] >= '0' && part[index] <= '9') || part[index] == '.'))
        ++index;
    while (index < part.size() && part[index] == ' ')
        ++index;
    if (index == part.size())
        return true;
    for (size_t rest = index; rest < part.size(); ++rest) {
        if (part[rest] == ' ')
            return false;
    }
    return true;
}

void SplitShoppingLines(const std::string& text, std::vector<std::string>& items) {
    items.clear();
    std::string line;
    for (char character : text) {
        if (character == '|' || character == '\n') {
            if (!line.empty()) {
                items.push_back(std::move(line));
                line.clear();
            }
        } else if (character != '\r') {
            line += character;
        }
    }
    if (!line.empty())
        items.push_back(std::move(line));
    if (items.size() == 2 && LooksLikeQuantity(items[0]) && !LooksLikeQuantity(items[1]))
        items = {items[0] + " " + items[1]};
}

void SplitShoppingSpoken(const std::string& spoken, std::vector<std::string>& items) {
    items.clear();
    const auto colon = spoken.rfind(':');
    std::string rest = colon == std::string::npos ? spoken : spoken.substr(colon + 1);
    while (!rest.empty() && (rest.back() == '.' || rest.back() == ' '))
        rest.pop_back();
    std::string piece;
    auto flush = [&]() {
        while (!piece.empty() && piece.front() == ' ')
            piece.erase(piece.begin());
        while (!piece.empty() && piece.back() == ' ')
            piece.pop_back();
        if (piece.size() >= 4 && (piece.compare(0, 4, "and ") == 0 || piece.compare(0, 4, "And ") == 0))
            piece.erase(0, 4);
        if (piece.empty())
            return;
        const std::string lower = [&] {
            std::string value = piece;
            for (char& character : value) {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character + ('a' - 'A'));
            }
            return value;
        }();
        if (lower.find(" more") != std::string::npos)
            return;
        items.push_back(std::move(piece));
        piece.clear();
    };
    for (char character : rest) {
        if (character == ',')
            flush();
        else
            piece += character;
    }
    flush();
}

void StoreShoppingFace(std::string& face, const std::vector<std::string>& items) {
    face.clear();
    for (const auto& item : items) {
        if (!face.empty())
            face += '|';
        face += item;
        if (face.size() > 500) {
            face.resize(500);
            break;
        }
    }
}

void FocusShoppingNewest(const std::vector<std::string>& items) {
    g_shopping_focus = items.empty() ? 0 : static_cast<uint8_t>(items.size() - 1);
    g_shopping_focus_since_us = esp_timer_get_time();
    g_shopping_scroll = 0;
}

void JoinShoppingLines(const std::vector<std::string>& items, size_t from, size_t to,
                       std::string& out) {
    out.clear();
    for (size_t index = from; index < to && index < items.size(); ++index) {
        if (!out.empty())
            out += '\n';
        out += items[index];
    }
}

void AppendShoppingItems(std::vector<std::string>& stored, std::string& face,
                         std::vector<std::string> items) {
    uint8_t added = 0;
    for (auto& item : items) {
        if (!stored.empty() && stored.back() == item)
            continue;
        stored.push_back(std::move(item));
        ++added;
    }
    if (stored.size() > 24)
        stored.erase(stored.begin(), stored.begin() + static_cast<long>(stored.size() - 24));
    g_shopping_appended = added;
    if (added)
        g_shopping_batch = added;
    StoreShoppingFace(face, stored);
    FocusShoppingNewest(stored);
}
}  // namespace

void Application::RememberShoppingListFace(const std::string& text) {
    if (text.empty() || text.size() > 500)
        return;
    // The notes face uses the same bar-separated shape. Those lines stay on
    // the notes list; a shopping paint must never absorb them.
    if (text.rfind("Notes", 0) == 0)
        return;
    std::string lower = text;
    for (char& character : lower) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
    if (lower.find("say the items") != std::string::npos ||
        lower.find("list unchanged") != std::string::npos ||
        lower.find("list unavailable") != std::string::npos ||
        lower.find("list off") != std::string::npos)
        return;
    if (lower == "shopping list" || lower == "recorded")
        return;
    const bool on_list = orbit_view_.load() == OrbitView::Shopping;
    if (!on_list && lower.find("shopping list") == std::string::npos &&
        lower.find("list empty") == std::string::npos && text.find('|') == std::string::npos)
        return;
    std::vector<std::string> items;
    if (text.find('|') != std::string::npos || text.find('\n') != std::string::npos)
        SplitShoppingLines(text, items);
    else if (on_list)
        items.push_back(text);
    if (items.empty())
        return;
    if (items.size() > 1 && LowerAscii(items.front()) == "shopping list") {
        // A read: the gateway sent the newest lines under a header. Replace
        // the stored list, and let the shorter spoken read-back leave it alone.
        items.erase(items.begin());
        shopping_list_items_ = std::move(items);
        StoreShoppingFace(shopping_list_face_, shopping_list_items_);
        g_shopping_appended = 0;
        g_shopping_batch = 0;
        g_shopping_read_face = true;
        FocusShoppingNewest(shopping_list_items_);
        if (on_list)
            PaintOrbitView();
        return;
    }
    AppendShoppingItems(shopping_list_items_, shopping_list_face_, std::move(items));
    if (on_list)
        PaintOrbitView();
}

void Application::RememberNotesFace(const std::string& text) {
    if (text.rfind("Notes", 0) != 0)
        return;
    if (text != "Notes" && text.rfind("Notes|", 0) != 0)
        return;
    notes_items_.clear();
    if (text.size() > 6) {
        std::string rest = text.substr(6);
        size_t start = 0;
        while (start < rest.size()) {
            const size_t bar = rest.find('|', start);
            const std::string line = rest.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!line.empty())
                notes_items_.push_back(line);
            if (bar == std::string::npos)
                break;
            start = bar + 1;
        }
    }
    g_notes_focus = notes_items_.empty()
                        ? 0
                        : static_cast<uint8_t>(std::min(notes_items_.size() - 1, size_t{255}));
    if (orbit_view_.load() == OrbitView::Notes)
        PaintOrbitView();
}

void Application::RememberShoppingListSpeech(const std::string& spoken) {
    if (spoken.empty() || spoken.size() > 500)
        return;
    std::string lower = spoken;
    for (char& character : lower) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
    if (lower.find("didn't catch") != std::string::npos ||
        lower.find("couldn't") != std::string::npos ||
        lower.find("isn't switched") != std::string::npos)
        return;
    if (lower.rfind("noted.", 0) == 0 || lower.rfind("on your notes", 0) == 0 ||
        lower.rfind("your notes are empty", 0) == 0)
        return;
    if (lower.find("empty") != std::string::npos) {
        shopping_list_items_.clear();
        shopping_list_face_.clear();
        g_shopping_focus = 0;
        g_shopping_focus_since_us = 0;
        g_shopping_scroll = 0;
        g_shopping_appended = 0;
        g_shopping_batch = 0;
        if (orbit_view_.load() == OrbitView::Shopping)
            PaintOrbitView();
        return;
    }
    std::vector<std::string> items;
    SplitShoppingSpoken(spoken, items);
    if (items.empty())
        return;
    const bool read_all = lower.find("on the shopping list") != std::string::npos;
    bool paint = true;
    if (read_all && g_shopping_read_face) {
        // The face already delivered the newest 24; the speech is a subset.
        g_shopping_read_face = false;
        paint = false;
    } else if (read_all) {
        shopping_list_items_ = std::move(items);
        g_shopping_appended = 0;
        g_shopping_batch = 0;
        StoreShoppingFace(shopping_list_face_, shopping_list_items_);
        g_shopping_focus = 0;
        g_shopping_focus_since_us = esp_timer_get_time();
        g_shopping_scroll = 0;
    } else if (g_shopping_appended > 0) {
        const uint8_t face_n = g_shopping_appended;
        g_shopping_appended = 0;
        if (items.size() > face_n) {
            const size_t have = shopping_list_items_.size();
            if (have >= face_n)
                shopping_list_items_.erase(shopping_list_items_.end() - static_cast<long>(face_n),
                                           shopping_list_items_.end());
            AppendShoppingItems(shopping_list_items_, shopping_list_face_, std::move(items));
        } else {
            FocusShoppingNewest(shopping_list_items_);
            paint = false;
        }
    } else {
        AppendShoppingItems(shopping_list_items_, shopping_list_face_, std::move(items));
    }
    if (paint && orbit_view_.load() == OrbitView::Shopping)
        PaintOrbitView();
}

void Application::PaintOrbitView() {
    auto* display = Board::GetInstance().GetDisplay();
    const auto view = orbit_view_.load();
    if (view == OrbitView::Home) {
        if (!dictation_screen_.load())
            display->SetDictationScreen(false, "", "");
        return;
    }
    if (view == OrbitView::Menu) {
        ProvisionsShowOrbitMenu(orbit_menu_index_);
        return;
    }
    if (view == OrbitView::Notes) {
        if (notes_items_.empty()) {
            ProvisionsShowShoppingFocus("", "hold to add", "", "Notes");
            return;
        }
        if (g_notes_focus >= notes_items_.size())
            g_notes_focus = static_cast<uint8_t>(notes_items_.size() - 1);
        const size_t index = g_notes_focus;
        std::string above;
        std::string below;
        if (index > 0)
            above = notes_items_[index - 1];
        if (index + 1 < notes_items_.size())
            below = notes_items_[index + 1];
        ProvisionsShowShoppingFocus(above, notes_items_[index], below, "Notes");
        return;
    }
    if (shopping_list_items_.empty() && !shopping_list_face_.empty())
        SplitShoppingLines(shopping_list_face_, shopping_list_items_);
    if (shopping_list_items_.empty()) {
        ProvisionsShowShoppingFocus("", "hold to add", "", "List");
        return;
    }
    if (g_shopping_focus >= shopping_list_items_.size())
        g_shopping_focus = static_cast<uint8_t>(shopping_list_items_.size() - 1);
    std::string above;
    std::string focus;
    std::string below;
    const size_t count = shopping_list_items_.size();
    if (g_shopping_scroll > 0) {
        // Scrolled page: five older items in white, hint that newer ones wait below.
        const size_t end = count - std::min(g_shopping_scroll, count - 1);
        const size_t start = end > kShoppingPage ? end - kShoppingPage : 0;
        JoinShoppingLines(shopping_list_items_, start, end, focus);
        below = "more below";
    } else if (g_shopping_batch > 0) {
        const size_t white = std::min(std::min(static_cast<size_t>(g_shopping_batch), count),
                                      kShoppingPage);
        const size_t white_from = count - white;
        const size_t prev = std::min(kShoppingPage - white, white_from);
        JoinShoppingLines(shopping_list_items_, white_from - prev, white_from, above);
        JoinShoppingLines(shopping_list_items_, white_from, count, focus);
    } else {
        const size_t index = g_shopping_focus;
        const size_t prev = std::min(kShoppingPageStep, index);
        JoinShoppingLines(shopping_list_items_, index - prev, index, above);
        focus = shopping_list_items_[index];
        const size_t after = index + 1 < count ? std::min(size_t{2}, count - index - 1) : 0;
        JoinShoppingLines(shopping_list_items_, index + 1, index + 1 + after, below);
    }
    ProvisionsShowShoppingFocus(above, focus, below, "List");
}
#endif
