"""Host checks for Orbit Lite mode: hello parsing, jitter buffer, faces, wiring."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"
APPLICATION = (MAIN / "application.cc").read_text(encoding="utf-8")
HEADER = (MAIN / "application.h").read_text(encoding="utf-8")
WEBSOCKET = (MAIN / "protocols/websocket_protocol.cc").read_text(encoding="utf-8")

# The vendor cJSON component is not vendored on the host. The lite hello header
# touches only these entry points, so a stub tree is enough to exercise it.
CJSON_STUB = r"""
#pragma once
#include <cstring>
struct cJSON {
    cJSON* next = nullptr;
    cJSON* child = nullptr;
    int type = 0;
    char* valuestring = nullptr;
    int valueint = 0;
    double valuedouble = 0;
    char* string = nullptr;
};
enum { cJSON_Number = 8, cJSON_String = 16, cJSON_Object = 64 };
inline bool cJSON_IsObject(const cJSON* item) { return item && item->type == cJSON_Object; }
inline bool cJSON_IsString(const cJSON* item) { return item && item->type == cJSON_String; }
inline bool cJSON_IsNumber(const cJSON* item) { return item && item->type == cJSON_Number; }
inline cJSON* cJSON_GetObjectItemCaseSensitive(const cJSON* object, const char* key) {
    if (!cJSON_IsObject(object)) return nullptr;
    for (cJSON* child = object->child; child; child = child->next)
        if (child->string && std::strcmp(child->string, key) == 0) return child;
    return nullptr;
}
"""


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_cpp(program: str) -> None:
    compiler = shutil.which("c++")
    if compiler is None:
        raise unittest.SkipTest("c++ compiler unavailable")
    with tempfile.TemporaryDirectory(prefix="orbit-lite-") as directory:
        path = Path(directory)
        (path / "cJSON.h").write_text(CJSON_STUB, encoding="utf-8")
        source = path / "lite.cc"
        binary = path / "lite"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run(
            [compiler, "-std=c++17", "-fsanitize=address,undefined", "-I", str(path),
             "-I", str(MAIN), "-I", str(MAIN / "protocols"), str(source), "-o", str(binary)],
            capture_output=True, text=True, check=False,
        )
        if built.returncode != 0:
            raise AssertionError(built.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20,
                                check=False)
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)


class LiteHelloTests(unittest.TestCase):
    def test_lite_hello_is_accepted_without_the_full_negotiation(self) -> None:
        run_cpp(
            r"""
#include <cassert>
#include <string>
#include <vector>
#include "provisions_lite_hello.h"
using namespace provisions::lite;

struct Tree {
    std::vector<cJSON*> nodes;
    ~Tree() { for (auto* n : nodes) { delete[] n->valuestring; delete[] n->string; delete n; } }
    cJSON* node(int type, const char* key) {
        auto* n = new cJSON; n->type = type;
        if (key) { n->string = new char[std::strlen(key) + 1]; std::strcpy(n->string, key); }
        nodes.push_back(n); return n;
    }
    cJSON* add(cJSON* parent, cJSON* child) {
        if (!parent->child) parent->child = child;
        else { auto* c = parent->child; while (c->next) c = c->next; c->next = child; }
        return child;
    }
    cJSON* object(cJSON* parent, const char* key) { return add(parent, node(cJSON_Object, key)); }
    cJSON* str(cJSON* parent, const char* key, const char* value) {
        auto* n = add(parent, node(cJSON_String, key));
        n->valuestring = new char[std::strlen(value) + 1]; std::strcpy(n->valuestring, value); return n;
    }
    cJSON* num(cJSON* parent, const char* key, double value) {
        auto* n = add(parent, node(cJSON_Number, key)); n->valuedouble = value; n->valueint = (int)value; return n;
    }
};

// The exact hello the lite gateway sends.
static cJSON* lite_hello(Tree& t, int rate, int duration, bool with_audio = true) {
    auto* root = t.node(cJSON_Object, nullptr);
    t.str(root, "type", "hello");
    t.str(root, "transport", "websocket");
    t.num(root, "version", 1);
    auto* provisions = t.object(root, "provisions");
    t.str(provisions, "mode", "lite");
    t.object(provisions, "selected");  // [] is an empty array; an empty object is the same for this parser
    if (with_audio) {
        auto* audio = t.object(root, "audio_params");
        t.str(audio, "format", "opus");
        t.num(audio, "sample_rate", rate);
        t.num(audio, "channels", 1);
        t.num(audio, "frame_duration", duration);
    }
    return root;
}

int main() {
    // 24 kHz / 60 ms mono Opus: the gateway's default, decoded natively.
    { Tree t; HelloParams p; assert(ParseLiteHello(lite_hello(t, 24000, 60), p) == HelloResult::kAccepted);
      assert(p.sample_rate == 24000 && p.frame_duration == 60 && p.session_id.empty()); }
    // 16 kHz is accepted too (rate converter path).
    { Tree t; HelloParams p; assert(ParseLiteHello(lite_hello(t, 16000, 60), p) == HelloResult::kAccepted);
      assert(p.sample_rate == 16000); }
    // No audio_params at all: stock defaults.
    { Tree t; HelloParams p; assert(ParseLiteHello(lite_hello(t, 0, 0, false), p) == HelloResult::kAccepted);
      assert(p.sample_rate == 24000 && p.frame_duration == 60); }
    // No timers_v1 / timer_claim_recovery_v1 / audio_capture / authenticated /
    // turn_ids / session UUID are required: the hello above carries none.
    // A session_id, when present, is kept verbatim (no UUID policy).
    { Tree t; auto* root = lite_hello(t, 24000, 60); t.str(root, "session_id", "lite-1");
      HelloParams p; assert(ParseLiteHello(root, p) == HelloResult::kAccepted && p.session_id == "lite-1"); }
    // Unsupported parameters are refused, never silently resampled to garbage.
    { Tree t; HelloParams p; assert(ParseLiteHello(lite_hello(t, 48000, 60), p) == HelloResult::kBadSampleRate); }
    { Tree t; HelloParams p; assert(ParseLiteHello(lite_hello(t, 24000, 100), p) == HelloResult::kBadFrameDuration); }
    { Tree t; auto* root = lite_hello(t, 24000, 60);
      auto* version = cJSON_GetObjectItemCaseSensitive(root, "version"); version->valuedouble = 2;
      HelloParams p; assert(ParseLiteHello(root, p) == HelloResult::kBadVersion); }
    // A full-gateway hello (no mode) hands over to the existing negotiation.
    { Tree t; auto* root = t.node(cJSON_Object, nullptr); auto* prov = t.object(root, "provisions");
      t.num(prov, "authenticated", 1); HelloParams p; assert(ParseLiteHello(root, p) == HelloResult::kNotLite); }
    { Tree t; auto* root = t.node(cJSON_Object, nullptr); auto* prov = t.object(root, "provisions");
      t.str(prov, "mode", "full"); HelloParams p; assert(ParseLiteHello(root, p) == HelloResult::kNotLite); }
    // The fixed lite conversation identity is non-zero (the recorder requires it).
    bool nonzero = false; for (auto byte : kConversationId) nonzero = nonzero || byte != 0; assert(nonzero);
}
"""
        )

    def test_actual_hello_short_circuits_before_any_gateway_requirement(self) -> None:
        hello = method(WEBSOCKET, "void WebsocketProtocol::ParseServerHello(const cJSON* root)")
        lite = hello.index("if (ParseLiteServerHello(root)) {")
        self.assertLess(lite, hello.index('"authenticated"'))
        self.assertLess(lite, hello.index("timers_v1"))
        self.assertLess(lite, hello.index("IsCanonicalUuid"))
        self.assertLess(lite, hello.index("output_fence_v1"))
        parse = method(WEBSOCKET, "bool WebsocketProtocol::ParseLiteServerHello(const cJSON* root)")
        # Only an unsupported audio parameter rejects; a lite hello never fails
        # for missing features, and the accepted path arms nothing.
        self.assertEqual(parse.count("RejectServerHello("), 1)
        self.assertIn("Invalid lite gateway audio parameters", parse)
        self.assertIn("timers_enabled_.store(false);", parse)
        self.assertIn("dictation_enabled_.store(false);", parse)
        self.assertIn("output_fence_selected_.store(false);", parse)
        self.assertIn("lite_mode_.store(true, std::memory_order_release);", parse)
        self.assertIn("gateway_authenticated_.store(true);", parse)
        self.assertIn("capture_context_.conversation_id = provisions::lite::kConversationId;", parse)
        self.assertIn("capture_enabled_.store(true);", parse)
        # Every open starts non-lite; the flag lives for the socket only.
        opened = WEBSOCKET.index("gateway_authenticated_.store(false);\n    lite_mode_.store(false);")
        self.assertLess(opened, WEBSOCKET.index("websocket->OnData("))
        # Lite frames skip the session_id / pong policing and reach the app.
        dispatch = WEBSOCKET[WEBSOCKET.index('RejectServerHello("Expected authenticated gateway hello");'):]
        dispatch = dispatch[:dispatch.index('"pong"')]
        self.assertIn("} else if (IsLiteMode()) {", dispatch)
        self.assertIn("on_incoming_json_(root);", dispatch)
        # No ping in lite and no heartbeat expiry.
        heartbeat = method(WEBSOCKET, "bool WebsocketProtocol::SendGatewayHeartbeat()")
        self.assertLess(heartbeat.index("if (IsLiteMode())"), heartbeat.index("ping"))
        expired = method(WEBSOCKET, "bool WebsocketProtocol::IsGatewayHeartbeatExpired() const")
        self.assertIn("if (IsLiteMode())\n        return false;", expired)


class LiteJitterBufferTests(unittest.TestCase):
    def test_jitter_buffer_state_machine(self) -> None:
        run_cpp(
            r"""
#include <cassert>
#include <memory>
#include <vector>
#include "provisions_lite_jitter.h"
using Buffer = provisions::lite::JitterBuffer<std::unique_ptr<int>>;
using State = Buffer::State;

struct Sink {
    std::vector<int> played;
    bool room = true;
    Buffer::Sink fn() {
        return [this](std::unique_ptr<int>& frame) {
            if (!room) return false;
            played.push_back(*frame);
            frame.reset();
            return true;
        };
    }
};
static std::unique_ptr<int> f(int n) { return std::make_unique<int>(n); }

int main() {
    // 1. Frames before `tts start` are held, not dropped; playback starts at
    //    3 frames once enabled.
    { Buffer b; Sink s; auto sink = s.fn();
      assert(b.Push(f(1), 0, sink).released == 0 && b.state() == State::kBuffering);
      assert(b.Push(f(2), 20, sink).released == 0);
      assert(b.Push(f(3), 40, sink).released == 0);
      assert(b.size() == 3 && s.played.empty());  // not enabled: nothing plays
      b.Enable();
      assert(b.Pump(41, sink) == 3 && b.state() == State::kPlaying);
      assert((s.played == std::vector<int>{1, 2, 3})); }

    // 2. Start after 400 ms with a single frame, and not before.
    { Buffer b; Sink s; auto sink = s.fn(); b.Enable();
      assert(b.Push(f(1), 1000, sink).released == 0);
      assert(b.Pump(1399, sink) == 0 && b.state() == State::kBuffering);
      assert(b.Pump(1400, sink) == 1 && b.state() == State::kPlaying);
      // Once playing, each frame streams straight through.
      assert(b.Push(f(2), 1460, sink).released == 1 && b.empty()); }

    // 3. Underrun: the sink drains while playing -> wait for 2 frames.
    { Buffer b; Sink s; auto sink = s.fn(); b.Enable();
      for (int i = 1; i <= 3; ++i) b.Push(f(i), i, sink);
      assert(b.state() == State::kPlaying && b.empty());
      assert(!b.OnSinkDrained() && b.state() == State::kRebuffering);
      assert(b.Push(f(4), 500, sink).released == 0 && b.state() == State::kRebuffering);
      assert(b.Push(f(5), 560, sink).released == 2 && b.state() == State::kPlaying);
      assert((s.played == std::vector<int>{1, 2, 3, 4, 5})); }

    // 4. Overflow: bounded at 50, oldest dropped, reported.
    { Buffer b; Sink s; auto sink = s.fn();  // never enabled: everything accumulates
      for (int i = 1; i <= 50; ++i) assert(b.Push(f(i), i, sink).dropped == 0);
      assert(b.size() == 50);
      assert(b.Push(f(51), 51, sink).dropped == 1 && b.size() == 50 && b.dropped_total() == 1);
      b.Enable(); b.Pump(52, sink);
      assert(s.played.size() == 50 && s.played.front() == 2 && s.played.back() == 51); }

    // 5. Flush on abort: Reset discards and disables.
    { Buffer b; Sink s; auto sink = s.fn(); b.Enable();
      b.Push(f(1), 0, sink); b.Push(f(2), 1, sink);
      b.Reset();
      assert(b.empty() && !b.enabled() && b.state() == State::kIdle);
      b.Push(f(3), 2, sink); b.Pump(1000, sink);
      assert(s.played.empty());  // still disabled after the abort
      b.Enable(); assert(b.Pump(1001, sink) == 1 && (s.played == std::vector<int>{3})); }

    // 6. Drain on stop: releases below the start threshold, finishes when the
    //    sink drains and nothing is held.
    { Buffer b; Sink s; auto sink = s.fn(); b.Enable();
      b.Push(f(1), 0, sink);
      assert(!b.Stop() && b.state() == State::kDraining);
      assert(b.Pump(1, sink) == 1 && b.empty());
      assert(b.OnSinkDrained());  // turn over
      // Stop with nothing held reports so immediately.
      Buffer c; c.Enable(); assert(c.Stop()); }

    // 7. A stray stop before any start (never enabled) just clears.
    { Buffer b; Sink s; auto sink = s.fn();
      b.Push(f(1), 0, sink); b.Push(f(2), 0, sink);
      assert(b.Stop() && b.empty() && b.state() == State::kIdle); }

    // 8. Sink refusal keeps the frame at the head, in order, until room.
    { Buffer b; Sink s; auto sink = s.fn(); b.Enable();
      for (int i = 1; i <= 3; ++i) b.Push(f(i), i, sink);
      s.room = false;
      assert(b.Push(f(4), 4, sink).released == 0 && b.size() == 1);
      assert(b.Push(f(5), 5, sink).released == 0 && b.size() == 2);
      s.room = true;
      assert(b.Pump(6, sink) == 2 && (s.played == std::vector<int>{1, 2, 3, 4, 5})); }

    // 9. Rebuffering while draining: stop releases everything regardless.
    { Buffer b; Sink s; auto sink = s.fn(); b.Enable();
      for (int i = 1; i <= 3; ++i) b.Push(f(i), i, sink);
      b.OnSinkDrained(); b.Push(f(4), 100, sink);
      assert(b.state() == State::kRebuffering && b.size() == 1);
      assert(!b.Stop()); assert(b.Pump(101, sink) == 1 && b.OnSinkDrained()); }
}
"""
        )

    def test_actual_thresholds_match_the_contract(self) -> None:
        jitter = (MAIN / "provisions_lite_jitter.h").read_text(encoding="utf-8")
        self.assertIn("kStartFrames = 3;", jitter)
        self.assertIn("kStartDelayMs = 400;", jitter)
        self.assertIn("kResumeFrames = 2;", jitter)
        self.assertIn("kCapacity = 50;", jitter)


class LiteFaceTests(unittest.TestCase):
    def test_face_mapping_and_text_bounds(self) -> None:
        run_cpp(
            r"""
#include <cassert>
#include <cstring>
#include <string>
#include "provisions_lite_face.h"
using namespace provisions::lite;

static bool same(const char* a, const char* b) { return (a == nullptr) == (b == nullptr) && (!a || !std::strcmp(a, b)); }
static void expect(const char* name, const char* status, const char* notification, const char* idle) {
    Face face; assert(ParseFace(name, face));
    const auto r = RenderFor(face);
    assert(same(r.status, status)); assert(same(r.notification, notification)); assert(same(r.idle_status, idle));
    assert(!r.status == !!r.notification);  // exactly one render path per face
}

int main() {
    // The mapping table from provisions_lite_face.h, one line each.
    expect("ready", "Ready", nullptr, nullptr);              // READY / HOLD TO TALK
    expect("working", "Working", nullptr, "Working");        // CHECKING / ONE MOMENT
    expect("speaking", "Speaking", nullptr, "Speaking");     // REPLY / LISTEN
    expect("recorded", nullptr, "Recorded", nullptr);        // RECORDED banner
    expect("timer", nullptr, "TIMER", nullptr);              // amber banner titled TIMER
    expect("failed", nullptr, "TRY AGAIN", nullptr);         // amber banner, back to READY
    expect("offline", "Unavailable", nullptr, "Unavailable"); // OFFLINE / TRY AGAIN
    Face face;
    assert(!ParseFace("", face) && !ParseFace("Ready", face) && !ParseFace("listening", face));
    Face ready; ParseFace("ready", ready); Face failed; ParseFace("failed", failed);
    Face offline; ParseFace("offline", offline); Face working; ParseFace("working", working);
    Face speaking; ParseFace("speaking", speaking); Face timer; ParseFace("timer", timer);
    assert(EndsTurn(ready) && EndsTurn(failed) && EndsTurn(offline));
    assert(!EndsTurn(working) && !EndsTurn(speaking) && !EndsTurn(timer));

    // Text: <= 40 bytes, control characters dropped, UTF-8 never split.
    assert(FaceText("").empty());
    assert(FaceText("Two kilos of tuna\r\n") == "Two kilos of tuna");
    assert(FaceText(std::string(40, 'a')).size() == 40);
    assert(FaceText(std::string(41, 'a')).size() == 40);
    const std::string euros = std::string(39, 'x') + "\xe2\x82\xac";  // 39 + 3 bytes
    const auto cut = FaceText(euros);
    assert(cut.size() == 39 && cut.back() == 'x');
    const std::string exact = std::string(37, 'x') + "\xe2\x82\xac";  // 40 bytes exactly
    assert(FaceText(exact) == exact);
}
"""
        )

    def test_actual_face_renderer_is_wired_to_the_display_and_idle_status(self) -> None:
        render = method(APPLICATION, "void Application::RenderLiteFace(")
        self.assertIn("lite_idle_status_.store(render.idle_status);", render)
        self.assertIn("display->ShowNotification(render.notification);", render)
        self.assertIn('display->SetChatMessage("assistant", text.c_str());', render)
        # A turn-ending face cuts playback and clears the reply watchdog.
        self.assertIn("if (provisions::lite::EndsTurn(face)) {", render)
        self.assertIn("SetProvisionsResponsePending(false);", render)
        idle = method(APPLICATION, "const char* Application::GetProvisionsIdleStatus() const")
        self.assertIn("if (const char* face = lite_idle_status_.load();", idle)
        self.assertIn('return provisions_response_pending_.load() ? "Working" : "Ready";', idle)
        self.assertLess(idle.index('return "Unavailable";'), idle.index("lite_idle_status_"))
        handler = method(APPLICATION, "void Application::HandleLiteGatewayFrame(")
        self.assertIn('strcmp(state->valuestring, "face") != 0', handler)
        self.assertIn("provisions::lite::ParseFace(name->valuestring, face)", handler)
        self.assertIn("provisions::lite::FaceText(text->valuestring)", handler)


class LiteWiringReview(unittest.TestCase):
    def test_actual_lite_frames_bypass_the_strict_gateway_checks(self) -> None:
        json = APPLICATION[APPLICATION.index("protocol->OnIncomingJson("):]
        json = json[:json.index("auto reject_gateway_frame")]
        self.assertIn("if (protocol->IsLiteMode()) {", json)
        self.assertIn("HandleLiteGatewayFrame(root, type->valuestring);", json)
        handler = method(APPLICATION, "void Application::HandleLiteGatewayFrame(")
        for state in ('"start"', '"stop"', '"sentence_start"'):
            self.assertIn(state, handler)
        self.assertIn('strcmp(type, "stt") == 0', handler)
        self.assertIn('strcmp(type, "provisions") == 0', handler)
        # Nothing in the lite handler can close the channel or alert.
        self.assertNotIn("reject_gateway_frame", handler)
        self.assertNotIn("CloseAudioChannel", handler)
        self.assertNotIn("Alert(", handler)
        # tts start reaches Speaking; stop drains rather than cutting.
        self.assertIn("SetDeviceState(kDeviceStateSpeaking);", handler)
        self.assertIn("const bool nothing_held = lite_playback_.Stop();", handler)
        self.assertIn("if (nothing_held && audio_service_.IsPlaybackIdle()) {", handler)

    def test_actual_audio_is_buffered_before_the_speaking_drop(self) -> None:
        audio = APPLICATION[APPLICATION.index("protocol->OnIncomingAudio("):]
        audio = audio[:audio.index("protocol->OnAudioChannelOpened(")]
        lite = audio.index("if (protocol->IsLiteMode()) {")
        self.assertLess(lite, audio.index("if (GetDeviceState() == kDeviceStateSpeaking) {"))
        self.assertLess(lite, audio.index("IsCurrentVoiceTurn"))
        self.assertIn("lite_playback_.Push(", audio[lite:])
        self.assertIn("dropped the oldest", audio[lite:])
        # The buffer is enabled only after the speaking transition reset the decoder.
        changed = method(APPLICATION, "void Application::HandleStateChangedEvent()")
        speaking = changed[changed.index("case kDeviceStateSpeaking:"):changed.index("case kDeviceStateNotifying:")]
        self.assertLess(speaking.index("audio_service_.ResetDecoder();"), speaking.index("lite_playback_.Enable();"))
        self.assertIn("StartLitePump();", speaking)
        # Drained event: finish after stop, refill/rebuffer otherwise.
        # Run() has preprocessor-unbalanced braces in raw text; slice by markers.
        run = APPLICATION[APPLICATION.index("void Application::Run()"):]
        drained = run[run.index("MAIN_EVENT_PLAYBACK_DRAINED) {"):run.index("MAIN_EVENT_TOGGLE_CHAT) {")]
        self.assertIn("if (lite_playback_.OnSinkDrained()) {", drained)
        self.assertIn("FinishLiteTurn();", drained)
        self.assertIn("PumpLitePlayback();", drained)
        # The sink never loses a frame for lack of room.
        sink = method(APPLICATION, "bool Application::PushLitePlayback(")
        self.assertLess(sink.index("HasDecodeQueueRoom()"), sink.index("PushPacketToDecodeQueue("))
        service = (MAIN / "audio/audio_service.cc").read_text(encoding="utf-8")
        room = method(service, "bool AudioService::HasDecodeQueueRoom()")
        self.assertIn("audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE", room)
        pump = re.search(r"kProvisionsLitePumpPeriodUs = (\d+) \* 1000;", APPLICATION)
        self.assertIsNotNone(pump)
        self.assertLessEqual(int(pump.group(1)), 200)

    def test_actual_press_aborts_and_flushes_then_records_on_the_same_hold(self) -> None:
        abort = method(APPLICATION, "void Application::AbortSpeaking(AbortReason reason)")
        self.assertIn("audio_service_.ResetDecoder();", abort)
        self.assertIn("ResetLitePlayback();", abort)
        self.assertLess(abort.index("ResetLitePlayback();"), abort.index("protocol->SendAbortSpeaking(reason);"))
        # The abort frame keeps the fork's shape.
        protocol = (MAIN / "protocols/protocol.cc").read_text(encoding="utf-8")
        send = method(protocol, "void Protocol::SendAbortSpeaking(AbortReason reason)")
        self.assertIn('"{\\"session_id\\":\\"" + this->session_id() + "\\",\\"type\\":\\"abort\\""', send)
        # Press-to-talk: the start-listening handler aborts first, then records.
        start = method(APPLICATION, "void Application::HandleStartListeningEvent()")
        self.assertLess(start.index("AbortSpeaking(kAbortReasonNone);"), start.index("BeginLocalRecordingOnMain()"))

    def test_actual_reply_watchdog_and_tts_deadline_stay_connected(self) -> None:
        timeout = re.search(r"kProvisionsLiteReplyTimeoutSeconds = (\d+);", APPLICATION)
        self.assertIsNotNone(timeout)
        self.assertEqual(int(timeout.group(1)), 12)
        maintenance = method(APPLICATION, "void Application::HandleProvisionsGatewayMaintenance()")
        watchdog = maintenance[maintenance.index("if (provisions_response_pending_.load() && IsLiteMode()) {"):]
        watchdog = watchdog[:watchdog.index("} else if (provisions_response_pending_.load()) {")]
        self.assertIn("kProvisionsLiteReplyTimeoutSeconds", watchdog)
        self.assertIn("RenderLiteFace(provisions::lite::Face::kFailed, {});", watchdog)
        self.assertNotIn("CloseAudioChannel", watchdog)
        self.assertNotIn("NoteProvisionsOffline", watchdog)
        deadline = maintenance[maintenance.index("&& IsLiteMode()) {"):maintenance.index('"Provisions TTS turn timed out"')]
        self.assertIn("FinishLiteTurn();", deadline)
        self.assertNotIn("CloseAudioChannel", deadline)
        # The full-gateway paths are untouched: the 30 s timeout and its close remain.
        self.assertIn("provisions_response_ticks_ >= kProvisionsResponseTimeoutSeconds", maintenance)
        self.assertIn('Alert("Unavailable", "Request timed out", "cancel", Lang::Sounds::OGG_EXCLAMATION);', maintenance)

    def test_actual_socket_loss_keeps_the_offline_buzz_and_drops_lite_playback(self) -> None:
        closed = APPLICATION[APPLICATION.index("protocol->OnAudioChannelClosed(["):]
        closed = closed[:closed.index("protocol->OnIncomingJson(")]
        self.assertIn("NoteProvisionsOffline();", closed)
        self.assertIn("ResetLitePlayback();", closed)
        self.assertLess(closed.index("NoteProvisionsOffline();"), closed.index("ResetLitePlayback();"))
        opened = APPLICATION[APPLICATION.index("protocol->OnAudioChannelOpened(["):]
        opened = opened[:opened.index("protocol->OnAudioChannelClosed([")]
        self.assertIn("ResetLitePlayback();", opened)
        self.assertIn("lite_idle_status_.store(nullptr);", opened)
        # PR #3 reconnect/buzz code is not modified beyond these calls.
        self.assertEqual(APPLICATION.count("kProvisionsOfflineBuzzMs"), 2)
        self.assertIn("Board::GetInstance().PulseHaptic(kProvisionsOfflineBuzzMs)", APPLICATION)

    def test_actual_lite_upload_retires_the_journal_slot_locally(self) -> None:
        send = method(APPLICATION, "void Application::SendVoiceRecording(")
        self.assertIn("if (protocol->IsLiteMode() && deferred) {", send)
        self.assertIn("if (sent && protocol->IsLiteMode())", send)
        self.assertEqual(send.count("AcknowledgeLiteUpload("), 2)
        ack = method(APPLICATION, "void Application::AcknowledgeLiteUpload(")
        for field in ("receipt.capture = replay->capture;", "receipt.bytes = replay->bytes;",
                      "receipt.digest = replay->digest;", "receipt.durable = true;"):
            self.assertIn(field, ack)
        self.assertIn("recorder->Acknowledge(receipt)", ack)
        # Header declarations exist for every lite member the source uses.
        for name in ("lite_playback_", "lite_playback_sink_", "lite_pump_timer_", "lite_pump_running_",
                     "lite_idle_status_", "PushLitePlayback", "PumpLitePlayback", "StartLitePump",
                     "StopLitePump", "ResetLitePlayback", "FinishLiteTurn", "HandleLiteGatewayFrame",
                     "RenderLiteFace", "AcknowledgeLiteUpload", "IsLiteMode"):
            self.assertIn(name, HEADER, name)
        self.assertIn('#include "provisions_lite_face.h"', HEADER)
        self.assertIn('#include "provisions_lite_jitter.h"', HEADER)


if __name__ == "__main__":
    unittest.main()
