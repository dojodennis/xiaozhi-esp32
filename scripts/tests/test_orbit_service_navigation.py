"""Exercise Service stop/menu ownership with the production queued callbacks."""

import unittest

from test_provisions_audio_boundaries import method, run_cpp


class ServiceNavigationTests(unittest.TestCase):
    def test_queued_stop_preserves_menu_and_explicit_mode_selection(self):
        navigation = "main/provisions_dictation_application.cc"
        service = "main/provisions_service_application.cc"
        production = "\n".join(
            [
                method(navigation, "void Application::HandleOrbitMenuBlueOnMain()"),
                method(navigation, "bool Application::ConfirmOrbitMenu()"),
                method(navigation, "void Application::ConfirmOrbitMenuOnMain()"),
                method(service, "void Application::StopOrbitServiceCapture()"),
                method(service, "void Application::PaintOrbitService()"),
                method(service, "void Application::SelectOrbitService()"),
            ]
        )
        # Compile the actual view-routing branches, excluding unrelated list data rendering.
        service_view = method(navigation, "if (view == OrbitView::Service)")
        menu_view = method(navigation, "if (view == OrbitView::Menu)")
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace provisions {
namespace dictation {
enum class State { Open, Reviewed };
enum class Action { None, Start, Stop };
struct Record { State state=State::Open; Action pending=Action::None; unsigned count=0; };
}
struct Recorder {
    dictation::Record record;
    bool continuous=true;
    unsigned stops=0;
    void SetContinuousDictation(bool enabled) { continuous=enabled; }
    dictation::Record DictationRecord() { return record; }
    void RequestDictationControl(dictation::Action action) {
        assert(action==dictation::Action::Stop); ++stops;
    }
    bool CaptureOpen() { return false; }
    unsigned PendingCount() { return 0; }
    bool DictationBusy() { return false; }
    bool DictationFaulted() { return false; }
};
}

struct Paint { std::string title, focus; int menu_page=-1; };
Paint painted;
unsigned paint_count=0;
void ProvisionsHideOrbitWifiSetup() {}
void ProvisionsShowOrbitMenu(uint8_t page) {
    painted={"Chef menu", "", page}; ++paint_count;
}
void ProvisionsShowShoppingFocus(const std::string&, const std::string& focus,
                                const std::string&, const std::string& title) {
    painted={title, focus, -1}; ++paint_count;
}
struct Display {
    std::string notification;
    void ShowNotification(const char* text) { notification=text; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
struct Settings {
    static inline unsigned writes=0;
    Settings(const char*, bool) {}
    void SetInt(const char*, int value) { assert(value==1); ++writes; }
};
struct WebsocketProtocol {
    bool opened=true;
    unsigned chef_requests=0, closed=0;
    bool IsAudioChannelOpened() { return opened; }
    bool RequestChefMode() { ++chef_requests; return opened; }
    void CloseAudioChannel() { opened=false; ++closed; }
};
struct Audio { bool IsLocalRecordingReady(unsigned) { return true; } };
struct Press { unsigned id() { return 1; } };
struct Application {
    enum class OrbitView : uint8_t { Home, Menu, Shopping, Notes, Stock, Service };
    std::atomic<OrbitView> orbit_view_{OrbitView::Service};
    std::atomic<bool> orbit_service_mode_{true}, orbit_service_recording_{true};
    std::atomic<bool> manual_listening_requested_{true}, provisions_network_busy_{false};
    std::atomic<bool> dictation_screen_{true};
    bool wifi=false, orbit_service_ready_=true;
    std::string orbit_service_code_, orbit_service_status_, orbit_service_table_="3";
    uint8_t orbit_menu_index_=4;
    unsigned provisions_reconnect_wait_ticks_=3, fenced=0, ended=0;
    std::shared_ptr<provisions::Recorder> provisions_recorder_=
        std::make_shared<provisions::Recorder>();
    std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();
    Audio audio_service_;
    Press provisions_physical_press_;
    std::vector<std::function<void()>> queue;
    bool IsOrbitService() { return orbit_service_mode_.load(); }
    bool IsOrbitWifiSetup() { return wifi; }
    void CancelOrbitWifiSetup() { wifi=false; }
    void LeaveDictationScreenOnMain() { dictation_screen_=false; }
    void StopListening() { manual_listening_requested_=false; ++fenced; }
    void EndLocalRecordingOnMain() { ++ended; }
    auto GetProtocol() { return protocol; }
    void Schedule(std::function<void()> fn) { queue.push_back(std::move(fn)); }
    void DrainBatch() {
        auto work=std::move(queue); queue.clear();
        for(auto& fn:work) fn();
    }
    void PaintOrbitView() {
        const auto view=orbit_view_.load();
        __SERVICE_VIEW__
        __MENU_VIEW__
    }
    void OpenOrbitShoppingOnMain() { orbit_view_=OrbitView::Shopping; }
    void OpenOrbitNotesOnMain() { orbit_view_=OrbitView::Notes; }
    void OpenOrbitStockOnMain() { orbit_view_=OrbitView::Stock; }
    void OpenOrbitTimersOnMain() { orbit_view_=OrbitView::Home; }
    void HandleOrbitMenuBlueOnMain();
    bool ConfirmOrbitMenu();
    void ConfirmOrbitMenuOnMain();
    void StopOrbitServiceCapture();
    void PaintOrbitService();
    void SelectOrbitService();
};

__PRODUCTION__

int main() {
    using View=Application::OrbitView;
    Application app;
    app.HandleOrbitMenuBlueOnMain();
    assert(app.orbit_view_==View::Menu && app.orbit_menu_index_==4);
    assert(painted.title=="Provisions" && painted.focus=="Chef");
    assert(app.fenced==1 && !app.orbit_service_recording_);
    assert(!app.provisions_recorder_->continuous && app.queue.size()==1);
    assert(app.ended==0 && app.provisions_recorder_->stops==0);
    const auto menu_paints=paint_count;
    // This is the actual deferred stop callback, not a synchronous navigation stub.
    app.DrainBatch();
    assert(app.ended==1 && app.provisions_recorder_->stops==1);
    assert(app.orbit_view_==View::Menu && paint_count==menu_paints);
    assert(painted.title=="Provisions" && painted.focus=="Chef");
    app.PaintOrbitService(); // Later status/discard callbacks also respect menu ownership.
    assert(paint_count==menu_paints);

    // Yellow explicitly requests Chef; only its later server ACK can change mode.
    assert(app.ConfirmOrbitMenu());
    assert(app.protocol->chef_requests==0 && app.queue.size()==1);
    app.DrainBatch();
    assert(app.protocol->chef_requests==1 && app.IsOrbitService());
    assert(app.orbit_view_==View::Menu && paint_count==menu_paints);
    assert(Board::GetInstance().display.notification=="Confirming Chef mode");
    assert(Settings::writes==0);
    app.protocol->opened=false;
    app.ConfirmOrbitMenu(); app.DrainBatch();
    assert(Board::GetInstance().display.notification=="Connect to switch to Chef");
    assert(app.IsOrbitService() && app.orbit_view_==View::Menu && paint_count==menu_paints);

    app.HandleOrbitMenuBlueOnMain();
    assert(app.orbit_view_==View::Service && painted.title=="Service");
    const auto service_paints=paint_count;
    app.wifi=true; app.PaintOrbitService(); assert(paint_count==service_paints);
    app.wifi=false; app.orbit_service_code_="pairing-code";
    app.PaintOrbitService(); assert(paint_count==service_paints);

    Application chef;
    chef.orbit_service_mode_=false;
    chef.orbit_service_recording_=false;
    chef.manual_listening_requested_=false;
    chef.orbit_view_=View::Menu;
    const View targets[]={View::Shopping,View::Home,View::Notes,View::Stock};
    for(uint8_t page=0;page<4;++page) {
        chef.orbit_view_=View::Menu; chef.orbit_menu_index_=page;
        chef.PaintOrbitView();
        assert(painted.title=="Chef menu" && painted.menu_page==page);
        chef.ConfirmOrbitMenu(); chef.DrainBatch();
        assert(chef.orbit_view_==targets[page] && !chef.IsOrbitService());
    }
    chef.orbit_view_=View::Menu; chef.orbit_menu_index_=4;
    chef.PaintOrbitView();
    assert(painted.title=="Dojo" && painted.focus=="Service" && Settings::writes==0);
    assert(chef.ConfirmOrbitMenu()); assert(!chef.IsOrbitService());
    chef.DrainBatch();
    assert(chef.IsOrbitService() && chef.orbit_view_==View::Service);
    assert(chef.dictation_screen_ && !chef.orbit_service_recording_);
    assert(Settings::writes==1 && chef.protocol->closed==1);
    assert(painted.title=="Service" && chef.provisions_recorder_->stops==0);
}
'''
        program = (
            program.replace("__PRODUCTION__", production)
            .replace("__SERVICE_VIEW__", service_view)
            .replace("__MENU_VIEW__", menu_view)
        )
        run_cpp(program)


if __name__ == "__main__":
    unittest.main()
