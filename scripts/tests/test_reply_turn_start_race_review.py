"""Compile the real talk handlers against overlapping physical-button events."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def extract_method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ReplyTurnStartRaceReview(unittest.TestCase):
    def test_newer_press_survives_send_and_microphone_start(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        application = (ROOT / "main/application.cc").read_text()
        header = (ROOT / "main/application.h").read_text()
        interrupted = extract_method(header, "bool ProvisionsReplyInterrupted() const")
        handlers = "\n".join(
            extract_method(application, f"void Application::{name}()")
            for name in (
                "StartListening",
                "StopListening",
                "HandleStartListeningEvent",
                "HandleStopListeningEvent",
                "StartListeningAudio",
            )
        )
        program = r'''
        #include <atomic>
        #include <cassert>
        #include <functional>
        #include <vector>
        #include "provisions_reply_turn.h"
        #define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
        #define MAIN_EVENT_START_LISTENING 1
        #define MAIN_EVENT_STOP_LISTENING 2
        #define ESP_LOGE(...) ((void)0)
        void xEventGroupSetBits(int, int) {}
        namespace Lang { namespace Sounds { constexpr int OGG_POPUP = 1; } }
        enum State { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking,
            kDeviceStateNotifying, kDeviceStateActivating, kDeviceStateWifiConfiguring,
            kDeviceStateAudioTesting, kDeviceStateConnecting };
        enum { kAbortReasonNone, kListeningModeManualStop };
        struct Audio {
            bool enabled = false;
            std::function<void()> during_enable;
            void EnableVoiceProcessing(bool value) {
                enabled = value;
                if (value && during_enable) during_enable();
            }
            void CloseVoiceUploadGate() { enabled = false; }
            void EnableAudioTesting(bool) {}
            void PlaySound(int) {}
        };
        struct Protocol {
            ProvisionsReplyTurn turn;
            std::function<void()> during_send;
            std::vector<unsigned> starts, stops;
            bool IsAudioChannelOpened() { return true; }
            void SendStartListening(int) {
                assert(turn.Begin());
                starts.push_back(turn.id());
                if (during_send) during_send();
            }
            void SendStopListening() { stops.push_back(turn.id()); }
            bool IsCurrentVoiceTurn(unsigned id) { return turn.IsCurrent(id); }
            unsigned voice_turn_id() { return turn.id(); }
            void InvalidateVoiceReply() { turn.Invalidate(); }
        };
        struct Application {
            ProvisionsReplyTurn provisions_physical_press_;
            std::atomic<unsigned> provisions_capture_press_{0};
            std::atomic<bool> manual_listening_requested_{false};
            std::atomic<bool> provisions_response_pending_{false};
            int event_group_ = 0, listening_mode_ = kListeningModeManualStop, aborts = 0;
            State state = kDeviceStateIdle;
            bool play_popup_on_listening_ = false;
            Protocol protocol;
            Protocol* protocol_ = &protocol;
            Protocol* GetProtocol() { return protocol_; }
            Audio audio_service_;
        ''' + interrupted + r'''
            State GetDeviceState() { return state; }
            void SetDeviceState(State value) { state = value; }
            void SetListeningMode(int) { state = kDeviceStateListening; }
            void StopNotification() { state = kDeviceStateIdle; }
            void InvalidateProvisionsTtsTurn() {}
            void SetProvisionsResponsePending(bool value) { provisions_response_pending_ = value; }
            void AbortSpeaking(int) {
                aborts++;
                provisions_capture_press_ = 0;
                protocol.InvalidateVoiceReply();
                provisions_response_pending_ = false;
                if (state == kDeviceStateSpeaking) state = kDeviceStateIdle;
            }
            void ContinueOpenAudioChannel(int mode) { SetListeningMode(mode); }
            void Schedule(std::function<void()> call) { call(); }
            void ConfigureWakeWordForListening() {}
            void StartListening(); void StopListening();
            void HandleStartListeningEvent(); void HandleStopListeningEvent();
            void StartListeningAudio();
        };
        ''' + handlers + r'''
        int main() {
            // Original reproduction: release + new press while the old start is sent.
            Application send_race;
            send_race.StartListening();
            send_race.HandleStartListeningEvent();
            send_race.protocol.during_send = [&] {
                send_race.StopListening();
                send_race.StartListening();
            };
            send_race.StartListeningAudio();
            assert(send_race.ProvisionsReplyInterrupted());
            assert(!send_race.protocol.IsCurrentVoiceTurn(1));
            assert(!send_race.audio_service_.enabled);
            assert(send_race.state == kDeviceStateIdle && send_race.aborts == 1);
            send_race.protocol.during_send = {};
            send_race.HandleStartListeningEvent();
            send_race.HandleStopListeningEvent(); // queued release belongs to old press
            assert(send_race.state == kDeviceStateListening);
            assert(send_race.protocol.stops.empty());
            send_race.StartListeningAudio();
            assert(!send_race.ProvisionsReplyInterrupted());
            assert(send_race.protocol.IsCurrentVoiceTurn(2));
            assert(send_race.audio_service_.enabled);
            send_race.StopListening();
            send_race.HandleStopListeningEvent();
            assert((send_race.protocol.starts == std::vector<unsigned>{1, 2}));
            assert((send_race.protocol.stops == std::vector<unsigned>{2}));
            assert(send_race.provisions_response_pending_);

            // A release during send must leave no microphone or phantom release.
            Application released;
            released.StartListening();
            released.HandleStartListeningEvent();
            released.protocol.during_send = [&] { released.StopListening(); };
            released.StartListeningAudio();
            released.HandleStopListeningEvent();
            assert(released.state == kDeviceStateIdle && released.aborts == 1);
            assert(released.protocol.stops.empty());
            assert(!released.provisions_response_pending_);
            assert(!released.audio_service_.enabled);

            // A newer press during microphone enable also keeps its fence.
            Application audio_race;
            audio_race.StartListening();
            audio_race.HandleStartListeningEvent();
            audio_race.audio_service_.during_enable = [&] {
                audio_race.StopListening();
                audio_race.StartListening();
            };
            audio_race.StartListeningAudio();
            assert(audio_race.ProvisionsReplyInterrupted());
            assert(!audio_race.audio_service_.enabled);
            audio_race.audio_service_.during_enable = {};
            audio_race.HandleStartListeningEvent();
            audio_race.HandleStopListeningEvent();
            assert(audio_race.state == kDeviceStateListening && audio_race.aborts == 1);
            assert(!audio_race.protocol.IsCurrentVoiceTurn(1));
            audio_race.StartListeningAudio();
            assert(audio_race.protocol.IsCurrentVoiceTurn(2));
            assert(!audio_race.ProvisionsReplyInterrupted());
            assert(audio_race.audio_service_.enabled);
            return 0;
        }
        '''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "review.cc"
            binary = Path(directory) / "review"
            source.write_text(program)
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-pthread", "-I", str(ROOT / "main"),
                 str(source), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
