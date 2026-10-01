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
void completed_retirement(){
 NvsStore s;Journal j(s);assert(j.Initialize());assert(j.Start(id(2),id(3)));assert(j.Apply(ack(j.Get())));
 assert(j.Reserve(id(10)));VoiceCapture c;c.purpose=VoicePurpose::Dictation;c.conversation_id=id(3);c.dictation_session_id=id(2);c.request_id=id(10);c.sample_count=160;
 assert(j.Seal(c));assert(j.Stop());assert(j.Apply(ack(j.Get(),false,State::Reviewed)));
 // A server completion without the exact segment receipt cannot retire audio.
 assert(!CanRetireEmpty(j.Get())&&!j.ReplaceEmpty(id(2),id(5),id(6)));
 assert(j.Terminal(c));assert(CanRetireEmpty(j.Get()));
 assert(!j.ReplaceEmpty(id(9),id(5),id(6)));assert(!j.ReplaceEmpty(id(2),id(5),id(3)));
 assert(j.ReplaceEmpty(id(2),id(5),id(6)));assert(j.Get().pending==Action::Start&&j.Get().count==0&&!j.Get().authorized);
 Journal reboot(s);assert(reboot.Initialize()&&reboot.Get().id==id(5)&&reboot.Get().conversation_id==id(6));
}
void expired_start_recovery(){
 for(bool stopped:{false,true}) {
  reset();NvsStore s;Journal j(s);assert(j.Initialize());assert(j.Start(id(2),id(3)));
  if(stopped)assert(j.Stop());
  auto receipt=ack(j.Get());receipt.expires_ms=1;
  assert(j.Apply(receipt));assert(!j.CanCapture(id(3),1788712345678LL));
  if(!stopped)assert(j.Stop());
  assert(j.Get().pending==Action::Stop&&!j.Get().authorized&&j.Get().frozen_count==0);
  auto end=ack(j.Get(),false,State::Reviewed);end.expires_ms=1;
  assert(j.Apply(end)&&j.Get().state==State::Reviewed&&!j.Get().authorized);
 }
}
int main(){reset();cycle();reset();pending_stop();reset();bounds();compact_corruption();reset();faults();reset();receipt_no_authority();wire_rejections();reset();completed_retirement();expired_start_recovery();std::cout<<"Dictation durable controls, CAS, reservations, restart and receipt authority passed\n";}
'''
class ShoppingSwipeScrollTest(unittest.TestCase):
    def test_swipe_pages_and_snaps_back(self):
        source = (ROOT / "main/provisions_dictation_application.cc").read_text(encoding="utf-8")
        self.assertIn("void Application::HandleShoppingSwipe(bool down)", source)
        self.assertIn("scroll = std::min(scroll + kShoppingPageStep, max_scroll);", source)
        self.assertIn("kShoppingScrollIdleUs = 8 * 1000000", source)
        # Any add, clear or read-all returns to the live (newest) page.
        self.assertGreaterEqual(source.count("g_shopping_scroll = 0;"), 4)
        self.assertIn('below = "more below";', source)
        board = (ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc").read_text(encoding="utf-8")
        self.assertIn("HandleShoppingSwipe(dy > 0)", board)
        self.assertIn("touch_swiped_ = true;", board)
        # One gesture per press, decided by the dominant axis at 40px: sideways
        # swaps shopping list <-> timer dial, up/down pages the list.
        self.assertIn("ax >= 40 || ay >= 40", board)
        self.assertIn("if (ax > ay) {", board)
        self.assertIn("HandleOrbitFaceSwipe(dx > 0)", board)
        self.assertIn("void Application::HandleOrbitFaceSwipe(bool right)", source)
        self.assertIn("OpenOrbitTimersOnMain()", source)
        self.assertIn("OpenOrbitShoppingOnMain()", source)
        touch = (ROOT / "main/boards/m5stack/stopwatch/cst820_touch.cc").read_text(encoding="utf-8")
        self.assertIn("((frame[5] & 0x0F) << 8) | frame[6]", touch)


class DictationTests(unittest.TestCase):
    def test_closing_the_dictation_screen_repaints_the_resting_face(self):
        # Hiding the panel only reveals the face underneath, so a stale
        # "Please try again" survived every visit to dictation.
        source = (ROOT / "main/provisions_dictation_application.cc").read_text(encoding="utf-8")
        service = source.split("void Application::ServiceDictation()", 1)[1]
        self.assertIn("SetDictationScreen(dictation_visible, status, action)", service)
        self.assertIn("DictationHeardFace", service)
        self.assertIn('"RECORDED"', source)
        self.assertIn('"TRY AGAIN"', source)
        self.assertIn('status = "Saving"', service)
        self.assertIn("dictation_review_pending_", service)
        self.assertNotIn("!recorder->DictationBusy()", service.split("const bool resume_stopped", 1)[0])
        resume = service.split("const bool resume_stopped", 1)[1].split("if (resume_stopped)", 1)[0]
        # A running countdown owns the output slot; dictation waits for it.
        self.assertIn("!timer_player_.Fenced()", resume)
        self.assertIn("const bool start_empty", service)
        self.assertIn('status = "Hold yellow to record"', service)
        self.assertNotIn("Paused - hold yellow", service)
        self.assertIn("r.pending == Action::Stop || r.pending == Action::Receipt", service.split("GetDeviceState() == kDeviceStateIdle", 1)[1].split("const auto control", 1)[0])
        app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        send = app.split("void Application::SendVoiceRecording(", 1)[1].split("auto protocol = GetProtocol();", 1)[0]
        self.assertNotIn("dictation_upload", send)
        self.assertIn("if (timer_player_.Fenced() && !alarm_stop)", send)
        self.assertIn("if (!dictation_screen_.load())", app)
        self.assertIn("board.GetDisplay()->ShowLocalCaptureReceipt();", app)
        recorded = app.split("if (result == Result::DictationRecorded)", 1)[1]
        self.assertIn("ServiceDictation();", recorded.split("return;", 1)[0])
        parse = app.split("gateway_turn = static_cast<uint32_t>(turn->valuedouble);", 1)[1]
        self.assertIn(
            "Talk-path guards belong on the paint callback",
            parse.split("if (strcmp(type->valuestring, \"provisions\")", 1)[0],
        )
        scheduled = app.split("message = std::move(display_text)]() {", 1)[1]
        self.assertIn("RememberShoppingListFace(message)", scheduled)
        self.assertIn("orbit_view_.load() == OrbitView::Shopping", scheduled.split("if (dictation_screen_.load())", 1)[0])
        overlay = scheduled.split("if (dictation_screen_.load())", 1)[1].split("return;", 1)[0]
        self.assertIn("dictation_heard_ = message", overlay)
        self.assertIn("SetProvisionsResponsePending(false)", overlay)
        self.assertNotIn("provisions_recording_started_press_ == 0", overlay)
        self.assertNotIn("message.find('|')", overlay)
        self.assertIn("Dictation review face:", app)
        self.assertIn("!dictation_screen_.load() &&", app)
        self.assertIn("dictation_screen_.load() || dictation_review_pending_", app)
        end_local = app.split("void Application::EndLocalRecordingOnMain()", 1)[1]
        stop_local = end_local.split("void Application::RetrySavedVoiceRecording()", 1)[0]
        self.assertIn('SetDictationScreen(true, "Saving", "Start")', stop_local)
        self.assertIn("dictation_heard_.empty() && dictation_heard_incoming_.empty()", stop_local)
        self.assertIn("if (!dictation_screen_.load())", stop_local)
        self.assertNotIn("ServiceDictation();", stop_local)
        self.assertNotIn("5000000", service)
        self.assertNotIn("1500000", service)
        self.assertNotIn("2000000", service)
        self.assertNotIn("3500000", service)
        self.assertNotIn("8000000", service)
        self.assertNotIn('"No text"', service)
        self.assertIn("AbortSpeaking(kAbortReasonNone)", source)
        self.assertIn("dictation_was_visible && !dictation_visible", service)
        self.assertIn("GetDeviceState() == kDeviceStateIdle", service)
        self.assertIn("SetStatus(GetProvisionsIdleStatus())", service)
        self.assertIn("Clip still sending - wait", service)
        self.assertIn('start_blocked() ? "Wait" : "Start"', service)
        self.assertIn("dictation_readback_.store(true)", app)
        self.assertIn("dictation_heard_voice_done_ = true", app)
        self.assertIn("GetDeviceState() == kDeviceStateIdle && !dictation_screen_.load() &&", app)
        self.assertIn("orbit_view_.load() == OrbitView::Home && !ProvisionsTimerFaceShowing()", app)
        self.assertIn("if (orbit_view_.load() == OrbitView::Menu)", source)
        self.assertIn("if (ProvisionsTimerFaceShowing())", source)
        speaking = app.split("case kDeviceStateSpeaking:", 1)[1]
        self.assertIn(
            "if (!dictation_screen_.load() && !IsOrbitShoppingFace() && !IsOrbitNotesFace())",
            speaking.split("if (listening_mode_", 1)[0],
        )
        self.assertIn("void Application::HandleOrbitMenuBlueOnMain()", source)
        self.assertIn("void Application::ConfirmOrbitMenuOnMain()", source)
        self.assertIn("return consumes_press", source)
        self.assertIn("IsOrbitNotesFace()", app)
        self.assertIn("manual_listening_requested_.load()", app)
        self.assertIn("ProvisionsShowOrbitMenu(orbit_menu_index_)", source)
        self.assertIn("OpenOrbitNotesOnMain()", source)
        self.assertIn("RememberNotesFace", source)
        self.assertIn('"hold to add"', source)
        self.assertIn("RememberShoppingListFace", source)
        self.assertIn('text.rfind("Notes", 0) == 0', source)
        self.assertIn('lower.rfind("noted.", 0) == 0', source)
        self.assertIn("text.find('|')", source)
        self.assertIn("LooksLikeQuantity", source)
        self.assertIn("items = {items[0] + \" \" + items[1]}", source)
        self.assertIn("AppendShoppingItems", source)
        self.assertIn("on the shopping list", source)
        self.assertIn("FocusShoppingNewest", source)
        self.assertIn("JoinShoppingLines", source)
        self.assertIn("g_shopping_batch", source)
        self.assertIn("items.size() > face_n", source)
        self.assertIn("g_shopping_scroll == 0 && g_shopping_batch == 0 &&\n            GetDeviceState() == kDeviceStateSpeaking", source)
        self.assertIn("ProvisionsShowTimerFace()", source)
        self.assertIn("SetListenShopping(orbit_view_.load() == OrbitView::Shopping)", app)
        self.assertIn("IsOrbitShoppingFace()", source)
        self.assertIn("RememberShoppingListSpeech", source)
        self.assertIn("RememberShoppingListSpeech(message)", app)
        self.assertIn("ProvisionsShowShoppingFocus", source)
        audio = app.split("protocol->OnIncomingAudio(", 1)[1].split(
            "protocol->OnAudioChannelOpened(", 1
        )[0]
        self.assertIn("!dictation_screen_.load() && !IsOrbitShoppingFace()", audio)
        start = app.split('strcmp(state->valuestring, "start") == 0', 1)[1]
        self.assertIn("!dictation_screen_.load() && !IsOrbitShoppingFace()", start)
        terminal = app.split("if (terminal)", 1)[1].split("const bool heard_ok", 1)[0]
        self.assertIn("IsOrbitShoppingFace()", terminal)
        idle = app.split("case kDeviceStateIdle:", 1)[1].split("case kDeviceStateConnecting:", 1)[0]
        self.assertIn("!dictation_screen_.load() && !IsOrbitShoppingFace()", idle)
        self.assertNotIn("PaintOrbitView()", idle)
        listening = app.split("case kDeviceStateListening:", 1)[1].split(
            "case kDeviceStateSpeaking:", 1
        )[0]
        self.assertIn("if (!IsOrbitShoppingFace() && !IsOrbitNotesFace())", listening)
        speaking = app.split("case kDeviceStateSpeaking:", 1)[1].split(
            "if (listening_mode_", 1
        )[0]
        self.assertIn(
            "!dictation_screen_.load() && !IsOrbitShoppingFace() && !IsOrbitNotesFace()",
            speaking,
        )
        wire = (ROOT / "main/provisions_voice_wire.cc").read_text(encoding="utf-8")
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text(encoding="utf-8")
        self.assertIn("alarm_stop, shopping, notes)", websocket)
        self.assertIn("ProvisionsListenNotes()", websocket)
        self.assertIn("ProvisionsListenShopping()", websocket)
        self.assertIn("bool ProvisionsListenShopping()", source)
        self.assertIn('cJSON_AddBoolToObject(root, "shopping", true)', wire)
        self.assertIn('cJSON_AddBoolToObject(root, "notes", true)', wire)

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


class ShoppingReadFaceTest(unittest.TestCase):
    def test_header_face_replaces_and_speech_leaves_it(self):
        source = (ROOT / "main/provisions_dictation_application.cc").read_text(encoding="utf-8")
        self.assertIn('LowerAscii(items.front()) == "shopping list"', source)
        self.assertIn("g_shopping_read_face = true;", source)
        self.assertIn("if (read_all && g_shopping_read_face) {", source)
