"""Render production Service UI with pinned LVGL/fonts, without hardware or data.

Source-extract the real Service creation block and state/render methods. Stubs
cover only the display mutex, unrelated surfaces, scheduler and hardware clock.
Emits PPM frames and checks round-screen bounds plus actual font extents.
"""
import argparse
from pathlib import Path
import subprocess
from test_provisions_audio_boundaries import method

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/m5stack/stopwatch'
SOURCE = 'main/boards/m5stack/stopwatch/m5stack_stopwatch.cc'

PREAMBLE = r'''
#include "lvgl.h"
#include "crest_asset.h"
#include "crest_audio.h"
#include "crest_motion.h"
#include "provisions_service_review.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
uint32_t clock_ms=1000;
struct Application {static Application& GetInstance(){static Application a;return a;}
bool IsOrbitStockFace(){return false;}};
namespace ProvisionsStopWatch {
constexpr char kStockReadyCaption[]="";
void StyleVoiceCaption(lv_obj_t*,bool){}
}
__PHASE__
struct RoundLcdDisplay {
    lv_obj_t *crest_layer_=nullptr,*crest_band_=nullptr,*crest_star_=nullptr,*crest_caption_=nullptr;
    lv_obj_t *service_panel_=nullptr,*service_body_=nullptr,*service_help_=nullptr;
    lv_obj_t *dictation_panel_=nullptr,*shopping_service_arc_=nullptr;
    std::array<lv_obj_t*,3> crest_rings_{};
    lv_timer_t* crest_animation_timer_=nullptr;
    OrbitCrest::State crest_state_=OrbitCrest::State::Boot;
    OrbitCrest::Progress crest_progress_=OrbitCrest::Progress::Command;
    OrbitCrest::Frame crest_frame_,crest_transition_from_;
    uint32_t crest_transition_ms_=0,crest_reply_started_ms_=0,crest_result_started_ms_=0,crest_result_hold_ms_=0;
    float crest_level_=0;
    bool crest_reply_received_=false,crest_speech_seen_=false,crest_error_ring_geometry_=false;
    bool service_layout_=false,shopping_focus_layout_=false,dictation_visible_=false,dictation_review_=false,dictation_saving_=false,orbit_locked_ui_=false;
    std::atomic<bool> power_save_active_{false},timer_alarm_active_{false},receipt_visible_{false},reply_visible_{false};
    std::string service_caption_text_,service_body_text_,service_help_text_,crest_displayed_caption_;
    const char* crest_result_caption_="";
    static uint32_t CrestNowMs(){return clock_ms;}
    bool Lock(int){return true;}void Unlock(){}
    void HideOrbitMenuLocked(){}void SetReplyLayoutLocked(bool){}void CancelVisualReset(){}
    static void SetVisible(lv_obj_t* obj,bool yes){if(!obj)return;if(yes)lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);}
    static void SetLabelText(lv_obj_t* obj,std::string* saved,const std::string& text){if(*saved!=text){*saved=text;lv_label_set_text(obj,text.c_str());}}
__METHODS__
};
'''
HOST = r'''
constexpr int size=466;
std::array<uint32_t,size*size> buffer{},frame{};
void flush(lv_display_t* d,const lv_area_t* a,uint8_t* pixels){
 auto* p=reinterpret_cast<uint32_t*>(pixels);
 for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x)frame[y*size+x]=*p++;
 lv_display_flush_ready(d);
}
void check_label(lv_obj_t* obj){
 if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))return;
 lv_area_t a;lv_obj_get_coords(obj,&a);
 // Caption and help rectangles remain within the circular glass.
 for(int x:{a.x1,a.x2})for(int y:{a.y1,a.y2})assert((x-233)*(x-233)+(y-233)*(y-233)<233*233);
 lv_point_t extent;
 lv_text_get_size(&extent,lv_label_get_text(obj),lv_obj_get_style_text_font(obj,LV_PART_MAIN),
     lv_obj_get_style_text_letter_space(obj,LV_PART_MAIN),lv_obj_get_style_text_line_space(obj,LV_PART_MAIN),
     lv_obj_get_content_width(obj),LV_TEXT_FLAG_NONE);
 if(extent.y>lv_obj_get_content_height(obj))std::fprintf(stderr,"clipped: %s extent=%d available=%d\n",lv_label_get_text(obj),extent.y,lv_obj_get_content_height(obj));
 assert(extent.y<=lv_obj_get_content_height(obj));
 assert(extent.x<=lv_obj_get_content_width(obj));
}
void emit(RoundLcdDisplay& ui,lv_display_t* d,const std::string& folder,const char* name,
          ProvisionsServicePhase phase,const char* status,const char* body,const char* help){
 assert(ui.ShowOrbitService(phase,status,body,help));
 clock_ms+=400;ui.RenderCrestLocked();lv_obj_update_layout(lv_screen_active());lv_refr_now(d);
 check_label(ui.crest_caption_);check_label(ui.service_help_);
 lv_area_t caption,hint;lv_obj_get_coords(ui.crest_caption_,&caption);lv_obj_get_coords(ui.service_help_,&hint);
 assert(caption.y2<hint.y1);
 if(!body[0]){
   // Actual transformed artwork and rings end before any status text starts.
   assert(lv_obj_get_y(ui.crest_band_)+348*lv_image_get_scale(ui.crest_band_)/256<caption.y1);
   for(auto* ring:ui.crest_rings_)assert(lv_obj_get_y(ring)+lv_obj_get_height(ring)<caption.y1);
 }else{
   assert(lv_obj_has_flag(ui.crest_band_,LV_OBJ_FLAG_HIDDEN));
   lv_area_t b;lv_obj_get_coords(ui.service_body_,&b);assert(caption.y2<b.y1&&b.y2<hint.y1);
 }
 auto path=folder+"/"+name+".ppm";FILE* f=std::fopen(path.c_str(),"wb");assert(f);
 std::fprintf(f,"P6\n466 466\n255\n");
 for(int y=0;y<size;++y)for(int x=0;x<size;++x){
   auto pixel=frame[y*size+x];if((x-233)*(x-233)+(y-233)*(y-233)>233*233)pixel=0x303030;
   unsigned char rgb[]={static_cast<unsigned char>(pixel>>16),static_cast<unsigned char>(pixel>>8),static_cast<unsigned char>(pixel)};
   assert(std::fwrite(rgb,1,3,f)==3);
 }
 assert(std::fclose(f)==0);
}
int main(int argc,char** argv){
 assert(argc==2);lv_init();auto* d=lv_display_create(size,size);
 lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
 lv_display_set_buffers(d,buffer.data(),nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_FULL);
 lv_display_set_flush_cb(d,flush);
 RoundLcdDisplay ui;ui.CreateCrestUiLocked(lv_screen_active());
 using P=ProvisionsServicePhase;
 emit(ui,d,argv[1],"01-connecting",P::Connecting,"Connecting to Dojo","","Please wait");
 emit(ui,d,argv[1],"02-ready",P::Ready,"Tap yellow to record","","Yellow: record\nHold blue: pair");
 emit(ui,d,argv[1],"03-sent",P::Ready,"Sent to Dojo\nReview at desk","","Yellow: record\nHold blue: pair");
 emit(ui,d,argv[1],"04-recording",P::Recording,"Recording","","yellow stops");
 emit(ui,d,argv[1],"05-lost",P::Lost,"Connection lost. Recording kept","","Retrying connection");
 emit(ui,d,argv[1],"06-transcript",P::Received,"Transcript received · part 1","Sample\nnote only","Swipe: read\nYellow: choose · Blue: next");
 emit(ui,d,argv[1],"07-recovery",P::Lost,"Sync recovery. Pair again afterward","","hold blue to pair · recording kept");
 emit(ui,d,argv[1],"08-unavailable",P::Lost,"Unable to connect to Dojo","","Retrying connection");
 emit(ui,d,argv[1],"09-destination",P::Received,"Choose destination\nGuest notes: desk review","General","Swipe to read destination\nBlue: change · Yellow: choose");
 emit(ui,d,argv[1],"10-confirm",P::Received,"Confirm this note\nGuest notes: desk review","General","Swipe to read destination\nYellow: confirm · Blue: cancel");
 emit(ui,d,argv[1],"11-saved",P::Saved,"Note saved · part 1","Sample note","Swipe: read\nYellow: record · Blue: next");
 ui.HideOrbitServiceLocked();lv_obj_update_layout(lv_screen_active());
 assert(lv_image_get_scale(ui.crest_band_)==256&&lv_obj_get_x(ui.crest_band_)==OrbitCrest::kBandX);
 assert(lv_obj_get_style_bg_opa(ui.crest_caption_,LV_PART_MAIN)==LV_OPA_TRANSP);
 std::puts("11 production LVGL frames: text extents, circle bounds, no overlaps, Chef geometry restored");
}
'''

def main():
    parser=argparse.ArgumentParser();parser.add_argument('output',type=Path);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    create=method(SOURCE,'void CreateCrestUiLocked(')
    create=create.split('        SetVisible(service_panel_, false);')[0]+'        SetVisible(service_panel_, false);\n    }'
    methods=[create]+[method(SOURCE,s) for s in ('void ClearCrestResultLocked(', 'void DismissSpokenFaceLocked(', 'void PositionCrestLocked(', 'void HideOrbitServiceLocked(', 'void ChangeCrestStateLocked(', 'void RenderCrestLocked(', 'bool ShowOrbitService(')]
    header=(ROOT/'main/display/display.h').read_text();phase='enum class ProvisionsServicePhase'+header.split('enum class ProvisionsServicePhase',1)[1].split('};',1)[0]+'};'
    (out/'host.cc').write_text(PREAMBLE.replace('__PHASE__',phase).replace('__METHODS__','\n'.join(methods))+HOST)
    (out/'sdkconfig.h').write_text('#pragma once\n#define CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_STOPWATCH 1\n#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n')
    managed=ROOT/'managed_components';fonts=managed/'78__xiaozhi-fonts/src'
    (out/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(orbit_service_render LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "{BOARD}/tests/lv_conf.h" CACHE PATH "" FORCE)
add_subdirectory("{managed}/lvgl__lvgl" lvgl)
add_executable(render host.cc "{BOARD}/crest_asset.cc" "{fonts}/font_noto_sans_basic_16_4.c" "{fonts}/font_noto_sans_basic_30_4.c")
target_include_directories(render PRIVATE "{out}" "{BOARD}" "{ROOT}/main" "{managed}/espressif__cjson/cJSON")
target_link_libraries(render PRIVATE lvgl)
target_compile_options(render PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>")
''')
    subprocess.run(['cmake','-S',str(out),'-B',str(out/'build')],check=True)
    subprocess.run(['cmake','--build',str(out/'build'),'-j','4'],check=True)
    subprocess.run([str(out/'build/render'),str(out)],check=True)

if __name__=='__main__':main()
