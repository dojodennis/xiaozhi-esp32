import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class OrbitTalkInterruptionTests(unittest.TestCase):
    def test_actual_talk_handler_interrupts_work_and_preserves_physical_capture_gate(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        application = (ROOT / "main/application.cc").read_text()
        handler = "void Application::HandleStartListeningEvent() {" + application.split(
            "void Application::HandleStartListeningEvent() {", 1
        )[1].split("void Application::HandleStopListeningEvent()", 1)[0]
        program = r'''
        #include <atomic>
        #include <cassert>
        #include <functional>
        #include "provisions_reply_turn.h"
        #define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
        #define ESP_LOGE(...) ((void)0)
        enum State { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking,
            kDeviceStateNotifying, kDeviceStateActivating, kDeviceStateWifiConfiguring,
            kDeviceStateAudioTesting, kDeviceStateConnecting };
        enum { kAbortReasonNone, kListeningModeManualStop };
        struct Audio { void EnableAudioTesting(bool) {} void EnableVoiceProcessing(bool) {} };
        struct Protocol { bool IsAudioChannelOpened() { return true; } };
        struct Application {
            std::atomic<bool> manual_listening_requested_{true};
            std::atomic<bool> provisions_response_pending_{false};
            State state = kDeviceStateIdle;
            Audio audio_service_;
            Protocol protocol;
            Protocol* protocol_ = &protocol;
            int aborts = 0;
            int invalidations = 0;
            bool ProvisionsReplyInterrupted() { return true; }
            State GetDeviceState() { return state; }
            void SetDeviceState(State value) { state = value; }
            void SetListeningMode(int) { state = kDeviceStateListening; }
            void StopNotification() { state = kDeviceStateIdle; }
            void InvalidateProvisionsTtsTurn() { invalidations++; }
            void AbortSpeaking(int) { aborts++; provisions_response_pending_ = false;
                if(state == kDeviceStateSpeaking) state = kDeviceStateIdle; }
            void ContinueOpenAudioChannel(int mode) { SetListeningMode(mode); }
            void Schedule(std::function<void()> call) { call(); }
            void HandleStartListeningEvent();
        };
        ''' + handler + r'''
        int main() {
            Application working;
            working.provisions_response_pending_ = true;
            working.HandleStartListeningEvent();
            assert(working.aborts == 1 && working.state == kDeviceStateListening);
            Application released;
            released.manual_listening_requested_ = false;
            released.provisions_response_pending_ = true;
            released.HandleStartListeningEvent();
            assert(released.aborts == 1 && released.state == kDeviceStateIdle);
            Application speaking;
            speaking.state = kDeviceStateSpeaking;
            speaking.HandleStartListeningEvent();
            assert(speaking.aborts == 1 && speaking.state == kDeviceStateListening);
            ProvisionsReplyTurn turn;
            assert(!turn.IsCurrent(0));
            assert(turn.Begin() && turn.IsCurrent(1));
            assert(turn.Begin() && turn.IsCurrent(2) && !turn.IsCurrent(1));
            return 0;
        }
        '''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.cc"
            binary = Path(directory) / "test"
            source.write_text(program)
            subprocess.run([compiler, "-std=c++17", "-pthread", "-I", str(ROOT / "main"),
                            str(source), "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
