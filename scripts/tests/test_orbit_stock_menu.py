"""Preserve four Chef faces and an explicit Service menu choice under queued input."""
import unittest
from test_provisions_audio_boundaries import method, run_cpp

class StockMenuTests(unittest.TestCase):
    def test_four_destinations_and_capture_route(self):
        names = ['bool Application::IsOrbitShoppingFace()', 'bool Application::IsOrbitNotesFace()',
                 'bool Application::IsOrbitStockFace()', 'bool Application::IsOrbitMenuFace()',
                 'void Application::HandleOrbitMenuBlueOnMain()',
                 'void Application::OpenOrbitShoppingOnMain()', 'void Application::OpenOrbitNotesOnMain()',
                 'void Application::OpenOrbitStockOnMain()', 'void Application::OpenOrbitTimersOnMain()',
                 'void Application::ConfirmOrbitMenuOnMain()', 'bool Application::ConfirmOrbitMenu()',
                 'void Application::HandleOrbitFaceSwipe(bool right)']
        production = '\n'.join(method('main/provisions_dictation_application.cc', name) for name in names)
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#include <string>
int g_shopping_scroll=4, timer_shown=0;
void ProvisionsShowTimerFace(){++timer_shown;}
void ProvisionsHideOrbitWifiSetup(){}
struct Display{void SetDictationScreen(bool,const char*,const char*){}};
struct Board{Display display;static Board& GetInstance(){static Board b;return b;}Display* GetDisplay(){return &display;}};
struct Application {
 enum class OrbitView:uint8_t{Home,Menu,Shopping,Notes,Stock,Service};
 std::atomic<OrbitView> orbit_view_{OrbitView::Home};
 std::atomic<bool> dictation_screen_{false};
 uint8_t orbit_menu_index_=0;bool wifi=false,service=false;int paints=0,cancels=0,selections=0,stops=0;
 std::atomic<bool> orbit_service_recording_{false};std::string orbit_service_code_;
 bool IsOrbitService(){return service;}void StopOrbitServiceCapture(){++stops;orbit_service_recording_=false;}
 void SelectOrbitService(){++selections;}
 std::vector<std::function<void()>> queue;
 bool IsOrbitShoppingFace() const;bool IsOrbitNotesFace() const;bool IsOrbitStockFace() const;bool IsOrbitMenuFace() const;
 bool IsOrbitWifiSetup(){return wifi;}void CancelOrbitWifiSetup(){wifi=false;++cancels;}
 void LeaveDictationScreenOnMain(){dictation_screen_=false;}
 void PaintOrbitView(){++paints;}
 void Schedule(std::function<void()> fn){queue.push_back(fn);}
 void Drain(){for(auto& fn:queue)fn();queue.clear();}
 void HandleOrbitMenuBlueOnMain();void OpenOrbitShoppingOnMain();void OpenOrbitNotesOnMain();
 void OpenOrbitStockOnMain();void OpenOrbitTimersOnMain();void ConfirmOrbitMenuOnMain();
 bool ConfirmOrbitMenu();void HandleOrbitFaceSwipe(bool);
};
__PRODUCTION__
int main(){
 Application a;
 using View=Application::OrbitView;
 a.HandleOrbitMenuBlueOnMain();assert(a.IsOrbitMenuFace());
 // Blue visits all four original choices plus the separate Service mode choice.
 for(int i=1;i<=5;++i){a.HandleOrbitMenuBlueOnMain();assert(a.orbit_menu_index_==i%5);}
 // Menu swipes visit all five choices without activating Service or opening a face.
 for(bool right:{true,false})for(int i=0;i<5;++i){
  auto before=a.orbit_menu_index_;a.HandleOrbitFaceSwipe(right);
  assert(a.orbit_menu_index_==before);a.Drain();
  assert(a.orbit_menu_index_==(before+(right?1:4))%5);assert(a.IsOrbitMenuFace());assert(a.selections==0);
 }
 const View views[]={View::Shopping,View::Home,View::Notes,View::Stock};
 for(int page=0;page<4;++page){
  a.orbit_view_=View::Menu;a.orbit_menu_index_=page;a.dictation_screen_=true;
  assert(a.ConfirmOrbitMenu()==(page==1));assert(a.IsOrbitMenuFace());a.Drain();
  assert(a.orbit_view_==views[page]);
  if(page==3){assert(!a.IsOrbitShoppingFace()&&!a.IsOrbitNotesFace());assert(!a.dictation_screen_);}
  assert(!a.ConfirmOrbitMenu());
 }
 // The fifth choice is explicit and never steals Stock's fourth slot.
 a.orbit_view_=View::Menu;a.orbit_menu_index_=4;
 assert(a.ConfirmOrbitMenu());assert(a.selections==0);a.Drain();assert(a.selections==1);
 a.orbit_view_=View::Stock;a.orbit_menu_index_=3;
 // Stock voice stays on the ordinary question route, and blue returns to its entry.
 a.HandleOrbitMenuBlueOnMain();assert(a.IsOrbitMenuFace()&&a.orbit_menu_index_==3);
 a.ConfirmOrbitMenu();a.Drain();assert(a.IsOrbitStockFace());
 a.HandleOrbitFaceSwipe(true);a.Drain();assert(a.IsOrbitShoppingFace());
 a.HandleOrbitFaceSwipe(false);a.Drain();assert(a.IsOrbitStockFace());
 a.HandleOrbitFaceSwipe(false);a.Drain();assert(a.IsOrbitNotesFace());
 // All four ordinary Chef face swipes stay in Chef; Service needs menu confirmation.
 for(bool right:{true,false})for(int i=0;i<4;++i){a.HandleOrbitFaceSwipe(right);a.Drain();assert(a.selections==1);}
 // Service blue stops once and returns to its explicit Chef switch choice.
 a.service=true;a.orbit_view_=View::Service;a.orbit_service_recording_=true;
 a.HandleOrbitMenuBlueOnMain();assert(a.stops==1&&a.IsOrbitMenuFace()&&a.orbit_menu_index_==4);
 assert(a.ConfirmOrbitMenu());a.Drain();assert(a.selections==2);
 a.HandleOrbitMenuBlueOnMain();assert(a.orbit_view_==View::Service&&a.stops==1);
 a.service=false;
 // Wi-Fi owns blue/confirm/swipe; no underlying page changes.
 a.wifi=true;auto view=a.orbit_view_.load();auto page=a.orbit_menu_index_;
 assert(a.ConfirmOrbitMenu());a.HandleOrbitFaceSwipe(true);a.Drain();
 assert(a.orbit_view_==view&&a.orbit_menu_index_==page);
 a.HandleOrbitMenuBlueOnMain();assert(a.cancels==1&&a.orbit_view_==view);
}
'''
        run_cpp(program.replace('__PRODUCTION__', production))
