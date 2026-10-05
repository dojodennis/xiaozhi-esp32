"""Run the production Service parser, page renderer and route guards with sanitizers."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_provisions_voice_wire_review import HEADERS
from test_provisions_audio_boundaries import method

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = r'''
#include "provisions_service_review.h"
#include "provisions_voice_wire.h"
#include <cJSON.h>
#include <cassert>
#include <fstream>
#include <memory>
#include <sstream>
using namespace provisions;
using namespace provisions::service;
VoiceReplay::~VoiceReplay() = default;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json parse(const std::string& value) { Json root(cJSON_Parse(value.c_str()),cJSON_Delete);assert(root);return root; }
VoiceId id(const char* value){VoiceId result;assert(ParseVoiceId(value,result));return result;}
const auto recording=id("11111111-2222-4333-8444-555555555555");
const auto binding=id("22222222-3333-4444-8555-666666666666");
const auto segment=id("33333333-4444-4555-8666-777777777777");
const auto visit=id("44444444-5555-4666-8777-888888888888");
const auto profile=id("55555555-6666-4777-8888-999999999999");
const auto request=id("66666666-7777-4888-8999-aaaaaaaaaaaa");
const auto other=id("77777777-8888-4999-8aaa-bbbbbbbbbbbb");
const std::string session="current-service-session";
std::string fixture(){return R"({"type":"dojo_service","state":"transcript","session_id":"current-service-session",
"binding_id":"22222222-3333-4444-8555-666666666666","recording_id":"11111111-2222-4333-8444-555555555555",
"revision":4,"recording_state":"stopped","complete":true,"expected_segments":2,"received_segments":2,
"segment_id":"33333333-4444-4555-8666-777777777777","sequence":0,"text_offset":0,"alias_offset":0,
"text":" Guest 1 likes coffee. ","full_text_length":23,"next_text_offset":null,"next_sequence":1,
"segment_saved_at":"2026-10-05T21:50:00Z","aliases":[{"visit_id":"44444444-5555-4666-8777-888888888888",
"visit_label":"Party A","guest_number":1,"profile_id":"55555555-6666-4777-8888-999999999999","label":"Guest 1 · Party A 44444444"}],
"next_alias_offset":null,"requires_desk_review":false,"review_elapsed_ms":30,"upload_to_preview_ms":100})";}
void replace(cJSON* root,const char* key,const std::string& value){auto item=parse(value);assert(cJSON_ReplaceItemInObjectCaseSensitive(root,key,item.release()));}
int main(int argc,char** argv){
    Snapshot s;auto root=parse(fixture());assert(ParseSnapshot(root.get(),session,s));
    assert(s.complete&&s.stopped&&s.text==" Guest 1 likes coffee. "&&s.aliases.size()==1);
    assert(!ParseSnapshot(root.get(),"old-session",s));
    for(const char* field:{"binding_id","recording_id","segment_id","sequence","revision","text_offset","alias_offset",
                          "text","complete","expected_segments","received_segments","full_text_length","aliases","requires_desk_review"}){
        auto bad=parse(fixture());replace(bad.get(),field,"null");assert(!ParseSnapshot(bad.get(),session,s));
        bad=parse(fixture());cJSON_AddNullToObject(bad.get(),field);assert(!ParseSnapshot(bad.get(),session,s));
    }
    for(const char* number:{"-1","1.5","2147483648","null","\"30\""}){
        auto bad=parse(fixture());replace(bad.get(),"review_elapsed_ms",number);assert(!ParseSnapshot(bad.get(),session,s));
    }
    auto wrong=parse(fixture());replace(wrong.get(),"next_text_offset","2");assert(!ParseSnapshot(wrong.get(),session,s));
    wrong=parse(fixture());replace(wrong.get(),"recording_state","\"open\"");assert(!ParseSnapshot(wrong.get(),session,s));
    wrong=parse(fixture());replace(wrong.get(),"received_segments","1");assert(!ParseSnapshot(wrong.get(),session,s));
    wrong=parse(fixture());replace(wrong.get(),"next_sequence","0");assert(!ParseSnapshot(wrong.get(),session,s));
    wrong=parse(fixture());replace(wrong.get(),"text",std::string("\"")+std::string(501,'a')+"\"");assert(!ParseSnapshot(wrong.get(),session,s));
    wrong=parse(fixture());auto array=cJSON_GetObjectItemCaseSensitive(wrong.get(),"aliases");
    cJSON_AddItemToArray(array,cJSON_Duplicate(array->child,true));assert(!ParseSnapshot(wrong.get(),session,s));
    assert(ParseSnapshot(root.get(),session,s));
    Review review;review.Reset(recording,binding,session);assert(review.Accept(s,session));
    assert(review.stage()==Stage::Preview&&review.target()==0&&review.GuestAllowed());
    review.MarkTextPage(0,1);review.MarkTargetPage(0,1);assert(review.Choose());assert(!review.Choose());review.NextTarget();assert(review.TargetLabel()=="General note");
    review.NextTarget();assert(review.TargetLabel().find("Party A")!=std::string::npos);review.MarkTextPage(0,1);review.MarkTargetPage(0,1);assert(review.Choose());
    auto regressed=s;regressed.complete=false;assert(!review.Accept(regressed,session));
    auto stale=s;stale.revision=3;assert(!review.Accept(stale,session));assert(review.stage()==Stage::Confirm);
    auto changed=s;changed.aliases[0].profile_id=other;assert(review.Accept(changed,session));
    assert(review.stage()==Stage::Preview&&review.target()==0);review.MarkTextPage(0,1);review.MarkTargetPage(0,1);assert(review.Choose());review.NextTarget();
    review.MarkTextPage(0,1);review.MarkTargetPage(0,1);assert(review.Choose());assert(review.Confirm(request));assert(review.stage()==Stage::Saving);
    const auto expected=review.pending();assert(expected.text=="Guest 1 likes coffee.");assert(!expected.guest);
    auto serial=parse(RouteJson(session,expected));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(serial.get(),"confirmed")));
    assert(!cJSON_GetObjectItemCaseSensitive(serial.get(),"profile_id"));
    auto reply=expected;reply.request_id=other;assert(!review.Saved(reply,session));assert(review.stage()==Stage::Saving);
    reply=expected;reply.segment_id=other;assert(!review.Saved(reply,session));
    reply=expected;reply.text="different";assert(!review.Saved(reply,session));
    reply=expected;reply.guest=true;assert(!review.Saved(reply,session));
    assert(!review.Saved(expected,"old-session"));assert(review.Saved(expected,session));assert(review.stage()==Stage::Saved);
    assert(!review.Saved(expected,session));
    review.Reset(other,binding,session);assert(!review.Accept(s,session));assert(review.stage()==Stage::None&&review.target()==0);
    review.Reset(recording,other,session);assert(!review.Accept(s,session));
    review.Reset(recording,binding,session);auto paged=s;paged.cursor.text_offset=20;assert(!review.Accept(paged,session));
    assert(review.Accept(s,session));review.Navigate({1,0,0});assert(!review.Accept(s,session));assert(review.target()==0);
    review.Reset(recording,binding,session);auto mixed=s;mixed.text="Guest 1 likes coffee; Guest 2 wants tea.";
    mixed.full_text_length=CharacterCount(mixed.text);assert(review.Accept(mixed,session));assert(!review.GuestAllowed());
    review.MarkTextPage(0,1);review.MarkTargetPage(0,1);assert(review.Choose());review.NextTarget();assert(review.target()==1);review.NextTarget();assert(review.target()==0);
    review.Reset(recording,binding,session);auto long_note=s;long_note.full_text_length=501;
    assert(review.Accept(long_note,session));assert(!review.GuestAllowed());
    review.Reset(recording,binding,session);auto flagged=s;flagged.requires_desk_review=true;
    assert(review.Accept(flagged,session));assert(!review.GuestAllowed());
    review.Reset(recording,binding,session);auto partial=s;partial.complete=false;partial.stopped=false;
    assert(review.Accept(partial,session));assert(!review.CanRoute()&&!review.Choose());
    review.Reset(recording,binding,session);auto aliases=s;aliases.next_alias_offset=16;
    assert(review.Accept(aliases,session));review.MarkTextPage(0,1);review.MarkTargetPage(0,1);assert(review.Choose());for(int i=0;i<3;++i)review.NextTarget();
    assert(review.MoreAliasesSelected());assert(!review.Choose());review.Navigate({0,0,16});assert(review.target()==0);
    auto reviewed=parse(ReviewJson(session,recording,{2,500,16}));assert(cJSON_GetObjectItemCaseSensitive(reviewed.get(),"text_offset")->valueint==500);
    for(const std::string& text:{std::string(500,'W'),std::string("\n\n\n\n\n\n\n\nA\nB\nC"),std::string("café ☕ guest one likes tea")}){
        uint32_t pages;std::string joined;PreviewPage(text,0,pages);
        for(uint32_t page=0;page<pages;++page)joined+=PreviewPage(text,page,pages);
        assert(joined==text);
    }
    review.Reset(recording,binding,session);auto wide=s;wide.aliases[0].label=std::string(70,'W');
    assert(review.Accept(wide,session));assert(!review.Choose());review.MarkTextPage(0,1);assert(review.Choose());
    review.NextTarget();review.NextTarget();assert(!review.Choose());
    uint32_t label_pages;PreviewPage(review.TargetLabel(),0,label_pages);assert(label_pages>1);
    review.MarkTargetPage(label_pages-1,label_pages);assert(!review.target_reviewed());
    std::string label_joined;
    for(uint32_t page=0;page<label_pages;++page){label_joined+=PreviewPage(review.TargetLabel(),page,label_pages);review.MarkTargetPage(page,label_pages);}
    assert(label_joined==wide.aliases[0].label&&review.target_reviewed());assert(review.Choose());assert(review.Confirm(request));
    // Read the actual gateway projection with both measured timing fields.
    if(argc>1){std::ifstream stream(argv[1]);assert(stream);std::stringstream buffer;buffer<<stream.rdbuf();auto frame=parse(buffer.str());
        const auto sid=cJSON_GetObjectItemCaseSensitive(frame.get(),"session_id");assert(ParseSnapshot(frame.get(),sid->valuestring,s));}
    for(int arg=2;arg<argc;++arg){std::ifstream stream(argv[arg]);assert(stream);std::stringstream buffer;buffer<<stream.rdbuf();auto frame=parse(buffer.str());
        const auto sid=cJSON_GetObjectItemCaseSensitive(frame.get(),"session_id");Route receipt;assert(ParseSaved(frame.get(),sid->valuestring,receipt));
        auto bad=Json(cJSON_Duplicate(frame.get(),true),cJSON_Delete);cJSON_AddNullToObject(bad.get(),"request_id");assert(!ParseSaved(bad.get(),sid->valuestring,receipt));
        if(arg==2){assert(!receipt.guest);assert(!cJSON_GetObjectItemCaseSensitive(frame.get(),"visit_id"));}else assert(receipt.guest&&receipt.alias.guest_number==2);
    }
}
'''

APP_STUB = r'''
#include <atomic>
#include <functional>
#define ESP_LOGI(...) ((void)0)
[[maybe_unused]] static constexpr char kServiceTag[]="OrbitService";
int64_t esp_timer_get_time(){return 1000;}
void esp_fill_random(void* data,size_t bytes){std::memset(data,0xAB,bytes);}
enum class ProvisionsServicePhase {Ready,Preparing,Recording,Processing,Received,Saving,Saved,Lost};
std::string display_body;bool display_accepts=true;unsigned display_calls=0;
bool ProvisionsShowOrbitService(ProvisionsServicePhase,const std::string&,const std::string& body,const std::string&){
    display_body=body;++display_calls;return display_accepts;
}
struct FakeRecorder {
    provisions::dictation::Record record;unsigned starts=0;
    auto DictationRecord(){return record;}
    bool DictationBusy(){return false;}bool DictationFaulted(){return false;}unsigned PendingCount(){return 0;}
    bool RequestDictationControl(provisions::dictation::Action){++starts;return true;}
    bool RequestEmptyDictationReplacement(const VoiceId&){++starts;return true;}
    void SetContinuousDictation(bool){}
};
struct WebsocketProtocol {
    bool opened=true;VoiceId approved=binding;
    std::string sid=session;auto session_id(){return sid;}
    bool IsAudioChannelOpened(){return opened;}
    bool GetServiceReviewBinding(VoiceId& out){out=approved;return opened;}
    bool DictationNegotiated(){return opened;}
    bool GetCaptureContext(VoiceContext& out){out.conversation_id=binding;return true;}
};
struct Audio {bool IsLocalRecordingReady(unsigned){return true;}};
struct Press {unsigned id(){return 1;}};
struct Fence {bool value=false;bool Fenced(){return value;}};
struct Application {
    enum class OrbitView{Service,Menu};std::atomic<OrbitView> orbit_view_{OrbitView::Service};
    std::atomic<bool> orbit_service_recording_{false},manual_listening_requested_{false};
    bool orbit_service_ready_=true,orbit_service_recovery_=false;
    std::string orbit_service_status_,orbit_service_code_;unsigned stopped=0;
    unsigned provisions_reconnect_wait_ticks_=0,orbit_service_text_page_=0,orbit_service_target_page_=0;
    int64_t orbit_service_review_send_us_=0,orbit_service_review_received_us_=0;Review orbit_service_review_;VoiceId orbit_service_review_dismissed_{};
    std::shared_ptr<FakeRecorder> provisions_recorder_=std::make_shared<FakeRecorder>();
    std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();
    Audio audio_service_;Press provisions_physical_press_;Fence timer_player_;
    std::vector<std::function<void()>> jobs;
    bool IsOrbitService(){return true;}bool IsOrbitWifiSetup(){return false;}
    auto GetProtocol(){return protocol;}void Schedule(std::function<void()> fn){jobs.push_back(std::move(fn));}
    void Drain(){while(!jobs.empty()){auto work=std::move(jobs);jobs.clear();for(auto& fn:work)fn();}}
    void StopOrbitServiceCapture(){orbit_service_recording_=false;++stopped;}
    void OrbitServiceReviewFrame(const cJSON*,const std::string&);
    void OrbitServiceNavigate(bool,bool=false);void OrbitServiceTap();void PaintOrbitService();
};
'''
APP_MAIN = r'''
int main(){
    Application app;app.provisions_recorder_->record.id=recording;
    app.provisions_recorder_->record.conversation_id=binding;
    app.provisions_recorder_->record.state=provisions::dictation::State::Reviewed;
    auto incoming=parse(fixture());app.OrbitServiceReviewFrame(incoming.get(),"old-session");assert(app.jobs.empty());
    display_accepts=false;app.OrbitServiceReviewFrame(incoming.get(),session);assert(app.orbit_service_review_.stage()==Stage::None);
    app.Drain();assert(app.orbit_service_review_.stage()==Stage::Preview&&!app.orbit_service_review_.text_reviewed());
    app.OrbitServiceTap();app.Drain();assert(app.orbit_service_review_.stage()==Stage::Preview);
    display_accepts=true;app.PaintOrbitService();assert(app.orbit_service_review_.text_reviewed());
    app.OrbitServiceTap();app.Drain();assert(app.orbit_service_review_.stage()==Stage::Target);
    app.OrbitServiceNavigate(true);app.Drain();assert(app.orbit_service_review_.target()==1);
    app.OrbitServiceTap();app.Drain();assert(app.orbit_service_review_.stage()==Stage::Confirm);
    app.OrbitServiceTap();app.Drain();assert(app.orbit_service_review_.stage()==Stage::Saving);
    auto route=parse(RouteJson(session,app.orbit_service_review_.pending()));
    cJSON_DeleteItemFromObjectCaseSensitive(route.get(),"action");cJSON_DeleteItemFromObjectCaseSensitive(route.get(),"confirmed");
    cJSON_AddStringToObject(route.get(),"state","note_saved");cJSON_AddStringToObject(route.get(),"binding_id",VoiceIdText(binding).c_str());
    cJSON_AddStringToObject(route.get(),"saved_at","2026-10-05T21:55:00Z");cJSON_AddBoolToObject(route.get(),"replayed",false);
    auto wrong=Json(cJSON_Duplicate(route.get(),true),cJSON_Delete);replace(wrong.get(),"request_id","\"77777777-8888-4999-8aaa-bbbbbbbbbbbb\"");
    app.OrbitServiceReviewFrame(wrong.get(),session);app.Drain();assert(app.orbit_service_review_.stage()==Stage::Saving);
    app.OrbitServiceReviewFrame(route.get(),session);app.Drain();assert(app.orbit_service_review_.stage()==Stage::Saved);
    // Starting another recording discards every previous destination in RAM.
    app.OrbitServiceTap();app.Drain();assert(app.orbit_service_recording_&&app.orbit_service_review_.stage()==Stage::None);
    assert(app.provisions_recorder_->starts==1);app.orbit_service_recording_=false;
    // Full long party labels are paged without changing their persistent identity.
    auto alias=cJSON_GetObjectItemCaseSensitive(incoming.get(),"aliases")->child;
    replace(alias,"label","\""+std::string(70,'W')+" stay\"");
    app.OrbitServiceReviewFrame(incoming.get(),session);app.Drain();app.OrbitServiceTap();app.Drain();
    app.OrbitServiceNavigate(true);app.Drain();app.OrbitServiceNavigate(true);app.Drain();
    assert(app.orbit_service_review_.target()==2&&!app.orbit_service_review_.target_reviewed());
    uint32_t pages;PreviewPage(app.orbit_service_review_.TargetLabel(),0,pages);assert(pages>1);
    app.OrbitServiceTap();app.Drain();assert(app.orbit_service_review_.stage()==Stage::Target);
    for(uint32_t page=1;page<pages;++page){app.OrbitServiceNavigate(true,true);app.Drain();}
    assert(app.orbit_service_review_.target_reviewed());app.OrbitServiceTap();app.Drain();assert(app.orbit_service_review_.stage()==Stage::Confirm);
    app.OrbitServiceNavigate(false,true);app.Drain();assert(app.orbit_service_review_.stage()==Stage::Confirm);
    // Old recording, binding and transport responses cannot resurrect a review.
    app.provisions_recorder_->record.id=other;app.OrbitServiceReviewFrame(incoming.get(),session);app.Drain();
    assert(app.orbit_service_review_.stage()==Stage::None);
    app.provisions_recorder_->record.id=recording;app.protocol->approved=other;
    app.OrbitServiceReviewFrame(incoming.get(),session);app.Drain();assert(app.orbit_service_review_.stage()==Stage::None);
    app.protocol->approved=binding;app.OrbitServiceReviewFrame(incoming.get(),session);app.protocol->sid="new-session";
    app.Drain();assert(app.orbit_service_review_.stage()==Stage::None);app.protocol->sid=session;
    // Session loss fences capture and blocks another start without reauthorizing.
    auto lost=parse("{\"type\":\"dojo_service\",\"state\":\"session_lost\",\"session_id\":\"current-service-session\"}");
    app.orbit_service_recording_=true;app.OrbitServiceReviewFrame(lost.get(),session);app.Drain();
    assert(!app.orbit_service_ready_&&!app.orbit_service_recording_&&app.stopped==1);
    app.OrbitServiceTap();app.Drain();assert(app.provisions_recorder_->starts==1);
}
'''

class ServiceReviewTests(unittest.TestCase):
    def test_preview_extent_fits_pinned_font_maximum_metrics(self):
        font=(ROOT/'managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_30_4.c').read_text()
        maximum=(max(int(value) for value in re.findall(r'\.adv_w = (\d+)',font))+15)//16
        height=int(re.search(r'\.line_height = (\d+)',font).group(1))
        self.assertLessEqual(maximum,40)
        self.assertLessEqual(height,43)
        header=(ROOT/'main/provisions_service_review.h').read_text()
        columns=int(re.search(r'kPreviewColumns = (\d+)',header).group(1))
        lines=int(re.search(r'kPreviewLines = (\d+)',header).group(1))
        width=int(re.search(r'kPreviewWidth = (\d+)',header).group(1))
        body_height=int(re.search(r'kPreviewHeight = (\d+)',header).group(1))
        padding=int(re.search(r'kPreviewPadding = (\d+)',header).group(1))
        spacing=int(re.search(r'kPreviewLineSpace = (\d+)',header).group(1))
        self.assertLessEqual(columns*maximum,width-2*padding)
        self.assertLessEqual(lines*height+(lines-1)*spacing,body_height-2*padding)

    def test_actual_application_dispatch_buttons_and_late_authority(self):
        methods="\n".join(method('main/provisions_service_application.cc',signature) for signature in (
            'void Application::OrbitServiceReviewFrame(', 'void Application::OrbitServiceNavigate(',
            'void Application::OrbitServiceTap()', 'void Application::PaintOrbitService()'))
        program=PROGRAM.split('int main(',1)[0]+APP_STUB+methods+APP_MAIN
        self.run_production(program, fixtures=False)

    def test_actual_parser_pages_and_correlated_explicit_routing(self):
        self.run_production(PROGRAM, fixtures=True)

    def run_production(self, program, fixtures):
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        self.assertTrue((cjson / 'cJSON.c').exists(), 'Canonical pinned dependencies required')
        self.assertTrue((ROOT/'scripts/tests/fixtures/orbit_service_gateway_transcript.json').exists(), 'Actual gateway projector fixture required')
        with tempfile.TemporaryDirectory(prefix='orbit-service-review-') as directory:
            path=Path(directory)
            for name,source in HEADERS.items():
                header=path/name;header.parent.mkdir(parents=True,exist_ok=True);header.write_text(source)
            (path/'review.cc').write_text(program)
            sanitize=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run([shutil.which('cc'),*sanitize,'-Wno-deprecated-declarations','-I',str(cjson),'-c',str(cjson/'cJSON.c'),'-o',str(path/'cjson.o')],check=True)
            binary=path/'review'
            subprocess.run([shutil.which('c++'),'-std=c++17','-Wall','-Wextra','-Werror',*sanitize,'-I',str(path),'-I',str(ROOT/'main'),'-I',str(cjson),str(path/'review.cc'),str(ROOT/'main/provisions_service_review.cc'),str(ROOT/'main/provisions_voice_wire.cc'),str(ROOT/'main/provisions_voice_recording.cc'),str(path/'cjson.o'),'-o',str(binary)],check=True)
            fixture=ROOT/'scripts/tests/fixtures/orbit_service_gateway_transcript.json'
            args=[str(binary)]
            if fixtures: args.append(str(fixture))
            for name in ('orbit_service_gateway_saved_general.json','orbit_service_gateway_saved_guest.json'):
                saved=ROOT/'scripts/tests/fixtures'/name
                self.assertTrue(saved.exists(),'Actual gateway receipt projection required')
                if fixtures: args.append(str(saved))
            subprocess.run(args,check=True)

if __name__=='__main__':unittest.main()
