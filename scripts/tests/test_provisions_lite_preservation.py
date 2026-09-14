"""Orbit Lite preservation: the real recorder worker under the lite transport.

An upload alone is never a receipt. A later authenticated, exact
capture_consumed acknowledgement may retire an ordinary Lite capture after its
answer completed; otherwise the bounded show-time eviction remains the only
non-durable erase. Connecting in Lite leaves the stored full-gateway context,
other recordings and dictation assignment proof intact. Runs the actual
VoiceRecorder/outbox/dictation sources through deterministic task/flash/NVS
shims.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "orbit_recorder_review_fixtures",
    Path(__file__).with_name("test_provisions_voice_recorder_review.py"))
review = importlib.util.module_from_spec(spec)
spec.loader.exec_module(review)

RECORDER = (ROOT / "main/provisions_voice_recorder.cc").read_text()
APPLICATION = (ROOT / "main/application.cc").read_text()
DICTATION_APP = (ROOT / "main/provisions_dictation_application.cc").read_text()

LITE_CASES = r'''
VoiceContext lite_context(){
    VoiceContext value;const char tag[]="orbit-lite-v1";
    std::copy_n(reinterpret_cast<const uint8_t*>(tag),13,value.conversation_id.begin());return value;
}
void use_lite(VoiceRecorder& recorder){assert(recorder.UseLiteContext(lite_context()));drain();assert(recorder.HasContext());}
void mark_uploaded(VoiceRecorder& recorder,const Offered& item,uint64_t uploaded_ms){
    VoiceReplay replay;replay.capture=item.receipt.capture;replay.bytes=item.receipt.bytes;replay.digest=item.receipt.digest;
    assert(recorder.MarkUploadedAwaitingReceipt(replay,uploaded_ms));drain();
}
std::string awaiting_key(size_t slot){return "lite_up_"+std::to_string(slot);}
uint64_t stored_sequence(size_t slot){
    auto found=state.nvs.find(awaiting_key(slot));if(found==state.nvs.end())return 0;
    uint64_t value=0;for(size_t i=0;i<8;++i)value|=uint64_t(found->second.at(i))<<(8*i);return value;
}
constexpr uint64_t kBaseMs=1788712345678ULL;
constexpr uint64_t kMinuteMs=60ULL*1000;
constexpr int64_t kMinuteUs=60LL*1000*1000;

void lite_upload_cases(){
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder); // full gateway, conversation 42
        record(recorder,1);assert(recorder.PendingCount()==1);
        const auto committed=state.nvs.at("context_v1");
        use_lite(recorder);
        // Connecting in lite touches neither NVS context nor the retained recording.
        assert(state.nvs.at("context_v1")==committed&&recorder.MatchesConversation(context().conversation_id));
        assert(recorder.PendingCount()==1&&recorder.AwaitingReceiptCount()==0);
        replay(recorder);assert(offered.empty()); // The full-gateway capture is never offered to lite.
        record(recorder,2);assert(recorder.PendingCount()==2);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Saved,uint32_t(2)));
        replay(recorder);assert(offered.size()==1&&offered.back().press==2);
        assert(offered.back().receipt.capture.conversation_id==lite_context().conversation_id);
        const auto lite_slot=offered.back().slot;
        const auto erases=state.erases;const auto synced=notice_count(VoiceRecorder::Result::Synced);
        // A successful local upload: kept in flash, marked, no Synced, nothing erased.
        mark_uploaded(recorder,offered.back(),kBaseMs+kMinuteMs);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Uploaded,uint32_t(2)));
        assert(state.erases==erases&&notice_count(VoiceRecorder::Result::Synced)==synced);
        assert(recorder.PendingCount()==2&&recorder.AwaitingReceiptCount()==1);
        assert(stored_sequence(lite_slot)!=0); // Persisted with the entry's journal sequence.
        // No re-offer every 30 s any more.
        clock_us+=30*1000000LL;replay(recorder);assert(offered.size()==1);
        clock_us+=30*1000000LL;replay(recorder);assert(offered.size()==1);
        // A later exact Lite completion retires only this ordinary capture.
        auto receipt=offered.back().receipt;receipt.consumed=true;acknowledge(recorder,receipt);
        assert(recorder.PendingCount()==1&&recorder.AwaitingReceiptCount()==0&&state.erases==erases+1);
        assert(notice_count(VoiceRecorder::Result::Consumed)==1&&
               notice_count(VoiceRecorder::Result::Synced)==synced&&stored_sequence(lite_slot)==0);
        // Dictation segments are never marked.
        VoiceReplay dictation;dictation.capture.purpose=VoicePurpose::Dictation;
        assert(!recorder.MarkUploadedAwaitingReceipt(dictation,kBaseMs));
        // Back on the full gateway the retained conversation-42 capture is offered again.
        authorize(recorder);clock_us+=30*1000000LL;replay(recorder);
        assert(offered.size()==2&&offered.back().receipt.capture.conversation_id==context().conversation_id);
    }
    join();
    // The awaiting-receipt state survives a reboot and keeps suppressing re-offers.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);use_lite(recorder);
        record(recorder,1);replay(recorder);assert(offered.size()==1);
        mark_uploaded(recorder,offered.back(),kBaseMs);assert(recorder.AwaitingReceiptCount()==1);
    }
    join();
    const auto erases_after_reboot=state.erases;
    {
        VoiceRecorder recorder;initialize(recorder);assert(recorder.HasContext()); // NVS context 42 still current
        assert(recorder.PendingCount()==1&&recorder.AwaitingReceiptCount()==1);
        use_lite(recorder);const auto before=offered.size();
        clock_us+=30*1000000LL;replay(recorder);assert(offered.size()==before);
        assert(recorder.PendingCount()==1&&state.erases==erases_after_reboot);
    }
    join();
    std::cout<<"Lite upload is retained until an exact completion acknowledgement\n";
}

void lite_deferred_cases(){
    // A capture whose press is gone (reboot) is still offered under Lite. An
    // upload does not retire it; only the exact completion acknowledgement does.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);use_lite(recorder);record(recorder,1);
    }
    join();
    const auto erases=state.erases;
    {
        VoiceRecorder recorder;initialize(recorder);use_lite(recorder);
        replay(recorder);assert(offered.size()==1&&offered.back().press==0);
        assert(recorder.PendingCount()==1&&state.erases==erases);
        // Offered again after the retry delay: still no local retirement.
        clock_us+=30*1000000LL;replay(recorder);assert(offered.size()==2&&offered.back().press==0);
        assert(recorder.PendingCount()==1&&state.erases==erases&&notice_count(VoiceRecorder::Result::Synced)==0);
    }
    join();
    std::cout<<"Deferred (reboot) capture is offered under lite and never retired without upload\n";
}

void lite_eviction_cases(){
    // Trusted-clock rule: only the OLDEST uploaded-awaiting-receipt command
    // capture at least 30 minutes old is evicted, only when the store is full.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);use_lite(recorder);
        for(uint32_t press=1;press<=4;++press)record(recorder,press,kBaseMs+press);
        assert(recorder.PendingCount()==4);
        replay(recorder);assert(offered.size()==1&&offered.back().press==1);
        mark_uploaded(recorder,offered.back(),kBaseMs);           // oldest, uploaded at base
        clock_us+=1000000;replay(recorder);assert(offered.size()==2&&offered.back().press==2);
        mark_uploaded(recorder,offered.back(),kBaseMs+kMinuteMs); // newer, uploaded a minute later
        assert(recorder.AwaitingReceiptCount()==2);
        const auto erases=state.erases;
        // Full store, nothing aged: Lite streams from RAM and evicts nothing.
        record(recorder,5,kBaseMs+10*kMinuteMs);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(5)));
        assert(recorder.PendingCount()==4&&state.erases==erases&&notice_count(VoiceRecorder::Result::Evicted)==0);
        // Un-uploaded entries (presses 3 and 4) are never candidates even when old.
        record(recorder,6,kBaseMs+31*kMinuteMs);
        // One eviction erase plus the journal's own erase-before-write of the freed slot.
        assert(notice_count(VoiceRecorder::Result::Evicted)==1&&state.erases>erases);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Saved,uint32_t(6)));
        bool evicted_oldest=false;
        for(const auto& notice:notices)if(notice.first==VoiceRecorder::Result::Evicted){evicted_oldest=notice.second==1;}
        assert(evicted_oldest&&recorder.PendingCount()==4&&recorder.AwaitingReceiptCount()==1);
        // Exactly one per full press: the next aged press evicts press 2, the next is RAM-only.
        record(recorder,7,kBaseMs+40*kMinuteMs);
        assert(notice_count(VoiceRecorder::Result::Evicted)==2&&recorder.AwaitingReceiptCount()==0&&recorder.PendingCount()==4);
        const auto settled=state.erases;
        record(recorder,8,kBaseMs+60*kMinuteMs); // Nothing uploaded remains: RAM-only, erase nothing.
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(8))&&state.erases==settled);
        assert(notice_count(VoiceRecorder::Result::Evicted)==2&&recorder.PendingCount()==4);
    }
    join();
    // No trusted clock (captured_unix_ms 0): the monotonic age since the mark decides.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);use_lite(recorder);
        for(uint32_t press=1;press<=4;++press)record(recorder,press,0);
        replay(recorder);assert(offered.size()==1);mark_uploaded(recorder,offered.back(),0);
        const auto erases=state.erases;
        record(recorder,5,0);assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(5)));
        clock_us+=29*kMinuteUs;record(recorder,6,0);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(6))&&state.erases==erases);
        clock_us+=2*kMinuteUs;record(recorder,7,0);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Saved,uint32_t(7))&&state.erases>erases);
        assert(notice_count(VoiceRecorder::Result::Evicted)==1&&recorder.PendingCount()==4&&recorder.AwaitingReceiptCount()==0);
    }
    join();
    // After a reboot the monotonic mark is unknown; without a trusted clock on
    // either side the entry is not evictable (conservative), so Lite uses RAM.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);use_lite(recorder);
        for(uint32_t press=1;press<=4;++press)record(recorder,press,0);
        replay(recorder);mark_uploaded(recorder,offered.back(),0);
    }
    join();
    const auto erases_rebooted=state.erases;
    {
        VoiceRecorder recorder;initialize(recorder);use_lite(recorder);assert(recorder.AwaitingReceiptCount()==1);
        clock_us+=60*kMinuteUs;record(recorder,5,0);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(5))&&state.erases==erases_rebooted);
    }
    join();
    std::cout<<"Eviction: only the oldest uploaded-awaiting-receipt command capture over 30 min, one per full press\n";
}

void lite_full_live_only_cases(){
    // A full store with retained, non-evictable data must not erase anything.
    // Authenticated Lite may instead use the bounded RAM replay allocation.
    fresh();
    {
        VoiceRecorder recorder;initialize(recorder);authorize(recorder);use_lite(recorder);
        for(uint32_t press=1;press<=4;++press)record(recorder,press,0);
        const auto retained=state.flash;const auto erases=state.erases;
        hold_replay=true;record(recorder,5,0);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(5)));
        assert(offered.size()==1&&offered.back().press==5&&offered.back().slot==VoiceOutbox::kSlots);
        assert(recorder.PendingCount()==4&&state.flash==retained&&state.erases==erases);
        // While the first RAM replay is owned by the uploader, a second press
        // fails closed instead of mutating or aliasing it.
        record(recorder,6,0);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::Failed,uint32_t(6)));
        assert(offered.size()==1&&state.flash==retained&&state.erases==erases);
        held.reset();hold_replay=false;record(recorder,7,0);
        assert(notices.back()==std::make_pair(VoiceRecorder::Result::LiveOnly,uint32_t(7)));
        assert(offered.size()==2&&offered.back().slot==VoiceOutbox::kSlots);
        assert(recorder.PendingCount()==4&&state.flash==retained&&state.erases==erases);
    }
    join();
    std::cout<<"Full Lite journal streams from bounded RAM without changing retained slots\n";
}

void lite_connect_cases(){
    // Application::ServiceDictation compiled from source: a lite socket keeps
    // the RAM assignment proof and names the missing route on the screen; a
    // full-gateway socket without dictation still clears it as before.
    fresh();
    {
        Application app;initialize(*app.provisions_recorder_);authorize(*app.provisions_recorder_);
        app.dictation_screen_=true;app.protocol->lite=true;app.protocol->negotiated=false;
        assert(app.dictation_has_assignment_proof_);
        app.ServiceDictation();
        assert(app.dictation_has_assignment_proof_&&app.dictation_assignment_proof_==context().conversation_id);
        const auto& status=Board::GetInstance().display.status;
        assert(status.rfind("Dictation needs the full gateway",0)==0);
        // Stored context and the recorder's proof-bearing conversation are untouched.
        assert(app.provisions_recorder_->MatchesConversation(context().conversation_id));
        assert(std::memcmp(state.nvs.at("context_v1").data(),"ORC1",4)==0);
        app.protocol->lite=false;app.protocol->negotiated=false;app.ServiceDictation();
        assert(!app.dictation_has_assignment_proof_);
    }
    join();
    std::cout<<"Connecting in lite keeps the assignment proof and the stored context\n";
}
'''


class LitePreservationTests(unittest.TestCase):
    def test_actual_recorder_keeps_lite_uploads_and_evicts_only_by_the_bounded_rule(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        crypto = Path("/opt/homebrew/opt/openssl@3")
        flags = ["-I", str(crypto / "include"), "-L", str(crypto / "lib")] if crypto.exists() else []
        worker = review.WORKER
        head = worker[:worker.index("int main(){")]
        program = (review.PRELUDE + review.SUPPORT + head + LITE_CASES +
                   "int main(){lite_upload_cases();lite_deferred_cases();lite_eviction_cases();"
                   "lite_full_live_only_cases();lite_connect_cases();return 0;}\n")
        with tempfile.TemporaryDirectory(prefix="orbit-lite-preservation-") as folder:
            path = Path(folder)
            for name, source in review.HEADERS.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(source)
            (path / "review.cc").write_text(program)
            binary = path / "review"
            cjson = ROOT / "managed_components/espressif__cjson/cJSON"
            if not (cjson / "cJSON.c").exists():
                raise unittest.SkipTest("managed_components/espressif__cjson is not present")
            subprocess.run(["cc", "-fsanitize=address,undefined", "-I", str(cjson), "-c",
                            str(cjson / "cJSON.c"), "-o", str(path / "json.o")], check=True)
            built = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson), *flags,
                 str(path / "review.cc"), str(ROOT / "main/provisions_voice_outbox.cc"),
                 str(ROOT / "main/provisions_voice_outbox_esp.cc"),
                 str(ROOT / "main/provisions_voice_recording.cc"),
                 str(ROOT / "main/provisions_voice_recorder.cc"),
                 str(ROOT / "main/provisions_dictation_recorder.cc"),
                 str(ROOT / "main/provisions_dictation.cc"),
                 str(ROOT / "main/provisions_dictation_store.cc"),
                 str(ROOT / "main/provisions_voice_wire.cc"),
                 str(ROOT / "main/provisions_timers.cc"),
                 str(ROOT / "main/provisions_hardware_facts.cc"), str(path / "json.o"), "-lcrypto",
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                                    env={**os.environ, "ASAN_OPTIONS":
                                         "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for line in ("Lite upload is retained until", "Deferred (reboot) capture", "Eviction: only the oldest",
                         "Full Lite journal streams from bounded RAM",
                         "Connecting in lite keeps"):
                self.assertIn(line, result.stdout)

    def test_source_erases_only_on_exact_completion_receipt_or_bounded_eviction(self):
        # The three callers are exact Lite completion, durable full-gateway
        # receipt, and bounded eviction. The upload mark itself never erases.
        self.assertEqual(RECORDER.count("RemoveAfterReceipt("), 3)
        receipt = RECORDER[RECORDER.index("void VoiceRecorder::ApplyReceipt("):RECORDER.index("void VoiceRecorder::Run()")]
        self.assertIn("if (receipt.consumed) {", receipt)
        self.assertIn("} else if (receipt.durable) {", receipt)
        self.assertIn("saved.capture.IsDictation()", receipt)
        self.assertIn("notify_(Result::Consumed, presses_[slot]);", receipt)
        mark = RECORDER[RECORDER.index("void VoiceRecorder::ApplyUploadMark("):RECORDER.index("bool VoiceRecorder::EvictForNewCapture(")]
        self.assertNotIn("RemoveAfterReceipt", mark)
        self.assertNotIn("Synced", mark)
        self.assertIn("saved.capture.IsDictation())\n            return;", mark)
        evict = RECORDER[RECORDER.index("bool VoiceRecorder::EvictForNewCapture("):RECORDER.index("VoiceStoreResult VoiceRecorder::Store(")]
        self.assertIn("if (!awaiting_receipt_[slot])\n            continue;", evict)
        self.assertIn("saved.capture.IsDictation())\n            continue;", evict)
        self.assertIn("kAwaitingReceiptEvictionUs", evict)
        self.assertIn("notify_(Result::Evicted, press);", evict)
        store = RECORDER[RECORDER.index("VoiceStoreResult VoiceRecorder::Store("):RECORDER.index("void VoiceRecorder::Run()")]
        self.assertIn("result == VoiceStoreResult::Full && EvictForNewCapture(", store)
        save = RECORDER[RECORDER.index("void VoiceRecorder::Save("):RECORDER.index("void VoiceRecorder::RefreshCount()")]
        self.assertIn("stored == VoiceStoreResult::Full && lite_active_.load()", save)
        self.assertIn("replay_.use_count() == 1", save)
        self.assertIn("notify_(Result::LiveOnly, work.press);", save)
        self.assertIn("replay_->slot = VoiceOutbox::kSlots;", save)
        header = (ROOT / "main/provisions_voice_recorder.h").read_text()
        self.assertIn("kAwaitingReceiptEvictionUs = 30LL * 60 * 1000 * 1000", header)
        # Replay suppression and the RAM-only lite context.
        self.assertIn("if (awaiting_receipt_[slot] || (attention_[slot] && !retry_pending_[slot]) ||", RECORDER)
        lite = RECORDER[RECORDER.index("bool VoiceRecorder::UseLiteContext("):RECORDER.index("bool VoiceRecorder::MarkUploadedAwaitingReceipt(")]
        for forbidden in ("SaveContext(", "PrepareContext(", "requested_context_", "nvs_"):
            self.assertNotIn(forbidden, lite, forbidden)
        self.assertIsNone(re.search(r"(?<![A-Za-z_])context_ =", lite))  # only lite_context_ is assigned
        self.assertIn("lite_active_.store(false);", RECORDER[RECORDER.index("bool VoiceRecorder::ActivateContext("):RECORDER.index("bool VoiceRecorder::UpdateContext(")])

    def test_application_routes_lite_through_the_preserving_paths(self):
        opened = APPLICATION[APPLICATION.index("protocol->OnAudioChannelOpened(["):APPLICATION.index("protocol->OnAudioChannelClosed([")]
        self.assertIn("? recorder->UseLiteContext(context)", opened)
        self.assertIn(": recorder->UpdateContext(context))", opened)
        handler = APPLICATION[APPLICATION.index("void Application::HandleVoiceRecordingResult("):APPLICATION.index("void Application::SendVoiceRecording(")]
        self.assertIn("result == Result::LiveOnly", handler)
        self.assertIn("result == Result::Uploaded", handler)
        self.assertIn("Sent. Kept on Orbit until confirmed.", handler)
        self.assertIn("result == Result::Evicted", handler)
        self.assertIn("ESP_LOGW", handler[handler.index("result == Result::Evicted"):])
        mark = APPLICATION[APPLICATION.index("void Application::MarkLiteUploaded("):APPLICATION.index("void Application::HandleVoiceRecordingResult(")]
        self.assertIn("replay->slot == provisions::VoiceOutbox::kSlots", mark)
        press = APPLICATION[APPLICATION.index("bool Application::BeginLocalRecordingOnMain()"):]
        press = press[:press.index("provisions_recording_started_press_ = press;")]
        lite = press[press.index("if (dictation_screen_.load()) {"):press.index("DictationAuthorization() != dictation_authorization_seen_")]
        self.assertIn("if (IsLiteMode()) {", lite)
        self.assertIn("return false;", lite)
        self.assertNotIn("BeginDictation(press", lite)
        service = DICTATION_APP[DICTATION_APP.index("void Application::ServiceDictation()"):]
        self.assertIn("const bool lite = connected && websocket->IsLiteMode();", service)
        self.assertIn("if (connected && !lite) {", service)
        self.assertIn('status = "Dictation needs the full gateway";', service)


if __name__ == "__main__":
    unittest.main()
