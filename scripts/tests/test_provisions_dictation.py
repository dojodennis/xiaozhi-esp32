"""Exercise the real dictation journal, NVS adapter and closed wire parser."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from test_provisions_timers import HEADERS as TIMER_HEADERS, PROGRAM as TIMER_PROGRAM
from test_provisions_voice_wire_review import HEADERS
ROOT = Path(__file__).resolve().parents[2]
NVS = TIMER_PROGRAM[TIMER_PROGRAM.index('std::string disk,pending;'):TIMER_PROGRAM.index('using Json=')]
NVS = NVS.replace('orbit_tmr_v1', 'orbit_dct_v1').replace('attempt', 'journal').replace('write_error=0;', '')
PROGRAM = r'''
#include "provisions_dictation.h"
#include "provisions_voice_wire.h"
#include <nvs.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <iostream>
using namespace provisions;
using namespace provisions::dictation;
VoiceReplay::~VoiceReplay() = default;
__NVS__
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json json(const std::string& text){Json out(cJSON_Parse(text.c_str()),cJSON_Delete);assert(out);return out;}
std::string uuid(unsigned n){char b[40];std::snprintf(b,sizeof(b),"00000000-0000-0000-0000-%012x",n);return b;}
VoiceId id(unsigned n){VoiceId out{};assert(ParseVoiceId(uuid(n).c_str(),out));return out;}
Reply ack(const Record& r, bool pending=false, State state=State::Open){
 auto request=json(ControlJson(r,uuid(1)));cJSON_ReplaceItemInObject(request.get(),"action",cJSON_CreateString(pending?"control_pending":"control_ack"));
 auto* payload=cJSON_DetachItemFromObject(request.get(),"payload");cJSON_AddItemToObject(request.get(),"control",payload);
 const char* action=r.pending==Action::Start?"start":r.pending==Action::Stop?"stop":r.pending==Action::Resume?"resume":"receipt";
 cJSON_AddStringToObject(request.get(),"control_action",action);
 if(!pending){auto* receipt=cJSON_AddObjectToObject(request.get(),"receipt");
 cJSON_AddStringToObject(receipt,"state",state==State::Open?"open":state==State::Stopped?"stopped":"reviewed");
 cJSON_AddNumberToObject(receipt,"revision",r.revision+1);
 cJSON_AddNumberToObject(receipt,"control_revision",r.pending==Action::Start?1:r.pending==Action::Receipt?r.control_revision:r.pending_revision+1);
 if(state==State::Open)cJSON_AddNullToObject(receipt,"expected_segments");else cJSON_AddNumberToObject(receipt,"expected_segments",r.count);
 cJSON_AddStringToObject(receipt,"expires_at","2026-10-01T00:00:00Z");}
 Reply out;assert(ParseReply(request.get(),uuid(1),out));return out;
}
void cycle(){
 NvsStore store;Journal j(store);assert(j.Initialize());assert(j.Start(id(2),id(3)));
 const auto start=disk;const auto start_json=RecordJson(j.Get());assert(!j.CanCapture(id(3),1));auto pending=ack(j.Get(),true);assert(!j.Apply(pending)&&disk==start);
 Journal reboot(store);assert(reboot.Initialize());assert(RecordJson(reboot.Get())==start_json);
 auto a=ack(reboot.Get());assert(reboot.Apply(a)&&reboot.CanCapture(id(3),1788712345678LL));assert(!reboot.CanCapture(id(4),1));assert(!reboot.CanCapture(id(3),0));assert(!reboot.CanCapture(id(3),reboot.Get().expires_ms));
 assert(reboot.Reserve(id(10)));VoiceCapture cap;cap.purpose=VoicePurpose::Dictation;cap.conversation_id=id(3);cap.dictation_session_id=id(2);cap.request_id=id(10);cap.sample_count=160000;
 assert(reboot.Stop());assert(reboot.Get().frozen_count==1&&reboot.Get().pending_revision==1&&!reboot.Get().authorized);
 const auto stop=ControlJson(reboot.Get(),uuid(1));assert(!reboot.Reserve(id(11)));assert(reboot.Seal(cap));assert(ControlJson(reboot.Get(),uuid(1))==stop);
 assert(!reboot.Apply(a));assert(reboot.Apply(ack(reboot.Get(),false,State::Stopped)));assert(reboot.Get().control_revision==2);
 assert(reboot.Resume());assert(reboot.Get().pending_revision==2&&!reboot.Get().authorized);
 auto resume=ack(reboot.Get());assert(reboot.Apply(resume)&&reboot.Get().control_revision==3&&reboot.Get().authorized);
 assert(reboot.Terminal(cap));assert(reboot.Get().count==1&&reboot.Get().segments[0].terminal);
 assert(reboot.Reserve(id(11)));assert(reboot.AbandonEmpty(id(11))&&reboot.Get().count==1);assert(!reboot.AbandonEmpty(id(10)));
 Record parsed;auto serialized=RecordJson(reboot.Get());assert(ParseRecord(serialized,parsed)&&RecordJson(parsed)==serialized);assert(!ParseRecord(serialized+"garbage",parsed));
 assert(reboot.Stop());auto latest=disk;assert(!reboot.Apply(resume)&&disk==latest);
}
void pending_stop(){NvsStore store;Journal j(store);assert(j.Initialize());assert(j.Start(id(2),id(3)));assert(j.Stop());assert(j.Get().pending==Action::Start&&j.Get().stop_requested);auto a=ack(j.Get());assert(j.Apply(a));assert(j.Get().pending==Action::Stop&&j.Get().pending_revision==1&&!j.Get().authorized);assert(j.Apply(ack(j.Get(),false,State::Stopped)));assert(j.Resume());assert(j.Stop());assert(j.Apply(ack(j.Get())));assert(j.Get().pending==Action::Stop&&j.Get().pending_revision==3&&!j.Get().authorized);}
void bounds(){NvsStore s;Journal j(s);assert(j.Initialize());assert(j.Start(id(2),id(3)));assert(j.Apply(ack(j.Get())));for(unsigned i=0;i<60;++i){assert(j.Reserve(id(100+i)));VoiceCapture cap;cap.purpose=VoicePurpose::Dictation;cap.conversation_id=id(3);cap.dictation_session_id=id(2);cap.request_id=id(100+i);cap.chunk_sequence=i;cap.sample_count=160000;assert(j.Seal(cap));}assert(!j.Reserve(id(200)));assert(j.Stop());assert(j.Get().frozen_count==60&&disk.size()==1512);Record r;assert(s.Load(r)==Store::LoadResult::Present&&r.count==60);}
void compact_corruption(){
 NvsStore store;const auto good=disk;assert(good.size()==1512);
 auto rejected=[&](const std::string& changed){disk=changed;Record r;assert(store.Load(r)==Store::LoadResult::Fault);Journal j(store);assert(!j.Initialize()&&j.Faulted());};
 for(size_t bytes=1;bytes<good.size();++bytes)rejected(good.substr(0,bytes));
 rejected(good+"x");
 for(size_t offset:{size_t(0),size_t(8),size_t(9),size_t(10),size_t(11),size_t(68),size_t(69),size_t(70),size_t(71),size_t(72+20),size_t(72+21),size_t(72+22),size_t(72+23)}){auto bad=good;bad[offset]=127;rejected(bad);}
 auto bad=good;std::fill_n(bad.begin()+36,16,0);rejected(bad);
 bad=good;std::copy_n(bad.begin()+72,16,bad.begin()+96);rejected(bad);
 bad=good;std::fill_n(bad.begin()+72+16,4,255);rejected(bad);
 bad=good;bad[24]=59;rejected(bad); // Frozen Stop must account for all60 ordinals.
 disk=good;Record r;assert(store.Load(r)==Store::LoadResult::Present&&r.count==60);
}
void faults(){for(int mode=0;mode<5;++mode){reset();NvsStore s;Journal j(s);assert(j.Initialize());if(mode==0)fail_open=true;if(mode==1)fail_set=true;if(mode==2)fail_commit=true;if(mode==3)corrupt_readback=true;if(mode==4)fail_read=true;assert(!j.Start(id(2),id(3)));assert(j.Faulted()&&!j.CanCapture(id(3),1));}reset();namespace_present=true;disk="broken";NvsStore s;Journal j(s);assert(!j.Initialize()&&j.Faulted());}
void wire_rejections(){
 const std::string valid=R"({"type":"dictation","action":"control_ack","session_id":"00000000-0000-0000-0000-000000000001","dictation_session_id":"00000000-0000-0000-0000-000000000002","control_action":"stop","control":{"control_revision":1,"expected_segments":0},"receipt":{"state":"stopped","revision":2,"control_revision":2,"expected_segments":0,"expires_at":"2026-10-01T00:00:00Z"}})";
 Reply reply;assert(ParseReply(json(valid).get(),uuid(1),reply));assert(!ParseReply(json(valid).get(),uuid(9),reply));
 for(int change=0;change<12;++change){auto root=json(valid);auto* control=cJSON_GetObjectItemCaseSensitive(root.get(),"control");auto* receipt=cJSON_GetObjectItemCaseSensitive(root.get(),"receipt");
  if(change==0)cJSON_AddBoolToObject(root.get(),"unknown",true);
  if(change==1)cJSON_AddStringToObject(root.get(),"session_id",uuid(1).c_str());
  if(change==2)cJSON_AddBoolToObject(control,"unknown",false);
  if(change==3)cJSON_AddNumberToObject(control,"control_revision",1);
  if(change==4)cJSON_ReplaceItemInObject(control,"expected_segments",cJSON_CreateNumber(61));
  if(change==5)cJSON_ReplaceItemInObject(control,"control_revision",cJSON_CreateNumber(1.5));
  if(change==6)cJSON_ReplaceItemInObject(receipt,"control_revision",cJSON_CreateNumber(3));
  if(change==7)cJSON_ReplaceItemInObject(receipt,"expected_segments",cJSON_CreateNumber(1));
  if(change==8)cJSON_ReplaceItemInObject(receipt,"expires_at",cJSON_CreateString("2026-02-30T00:00:00Z"));
  if(change==9)cJSON_ReplaceItemInObject(receipt,"revision",cJSON_CreateNumber(0));
  if(change==10)cJSON_ReplaceItemInObject(root.get(),"dictation_session_id",cJSON_CreateString(uuid(0).c_str()));
  if(change==11)cJSON_ReplaceItemInObject(receipt,"state",cJSON_CreateString("open"));
  assert(!ParseReply(root.get(),uuid(1),reply));
 }
}
void receipt_no_authority(){NvsStore s;Journal j(s);assert(j.Initialize());assert(j.Start(id(2),id(3)));assert(j.Apply(ack(j.Get())));assert(j.Stop());assert(j.Apply(ack(j.Get(),false,State::Stopped)));assert(j.RequestReceipt());auto r=ack(j.Get());assert(j.Apply(r));assert(j.Get().state==State::Open&&!j.Get().authorized&&!j.CanCapture(id(3),1));}
int main(){reset();cycle();reset();pending_stop();reset();bounds();compact_corruption();reset();faults();reset();receipt_no_authority();wire_rejections();std::cout<<"Dictation durable controls, CAS, reservations, restart and receipt authority passed\n";}
'''
class DictationTests(unittest.TestCase):
    def test_closing_the_dictation_screen_repaints_the_resting_face(self):
        # Hiding the panel only reveals the face underneath, so a stale
        # "Please try again" survived every visit to dictation.
        source = (ROOT / "main/provisions_dictation_application.cc").read_text(encoding="utf-8")
        service = source.split("void Application::ServiceDictation()", 1)[1]
        self.assertIn("SetDictationScreen(dictation_visible, status, action)", service)
        self.assertIn("dictation_was_visible && !dictation_visible", service)
        self.assertIn("GetDeviceState() == kDeviceStateIdle", service)
        self.assertIn("SetStatus(GetProvisionsIdleStatus())", service)

    def test_actual_journal_nvs_and_wire(self):
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        with tempfile.TemporaryDirectory(prefix='orbit-dictation-') as folder:
            path=Path(folder)
            for name, source in {
                **HEADERS,
                'nvs.h': TIMER_HEADERS['nvs.h'],
                'esp_random.h': TIMER_HEADERS['esp_random.h'],
            }.items():
                target=path/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(source)
            (path/'test.cc').write_text(PROGRAM.replace('__NVS__',NVS))
            sanitize=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(['cc',*sanitize,'-I',str(cjson),'-c',str(cjson/'cJSON.c'),'-o',str(path/'json.o')],check=True,capture_output=True)
            result=subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',*sanitize,'-I',str(path),'-I',str(ROOT/'main'),'-I',str(cjson),str(path/'test.cc'),*[str(ROOT/'main'/name) for name in ('provisions_dictation.cc','provisions_dictation_store.cc','provisions_timers.cc','provisions_voice_wire.cc','provisions_voice_recording.cc')],str(path/'json.o'),'-o',str(path/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(path/'test')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0' if sys.platform=='darwin' else 'detect_leaks=1'})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
