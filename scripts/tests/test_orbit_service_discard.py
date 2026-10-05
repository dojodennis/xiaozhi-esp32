"""Exercise the real authenticated discard routing and immediate sample fence."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_provisions_local_capture_integration_review import method
from test_provisions_voice_wire_review import HEADERS

ROOT = Path(__file__).resolve().parents[2]


class ServiceDiscardFrameTests(unittest.TestCase):
    def test_authenticated_exact_record_fences_before_main_work(self):
        app = (ROOT / "main/application.cc").read_text()
        service = (ROOT / "main/provisions_service_application.cc").read_text()
        policy = (ROOT / "main/provisions_endpoint_policy.cc").read_text()
        route = method(app, 'if (IsOrbitService() && strcmp(type->valuestring, "dojo_service") == 0)')
        discard = method(service, 'if (state == "discarded")')
        program = r'''
#include "provisions_voice_wire.h"
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>
using namespace provisions;
VoiceReplay::~VoiceReplay() = default;
namespace ProvisionsEndpointPolicy { __HEX__ __UUID__ }
struct FakeRecorder {
 dictation::Record record;bool pending=false;unsigned requests=0;
 dictation::Record DictationRecord(){return record;}
 bool DictationDiscardPending(){return pending;}
 void SetContinuousDictation(bool enabled){assert(!enabled);}
 bool RequestDiscardedDictation(const VoiceId& id){if(id!=record.id)return false;++requests;pending=true;return true;}
};
struct WebsocketProtocol {
 unsigned interrupts=0;std::string session_id(){return "current-session";}
 bool ServiceReviewNegotiated(){return false;}
 void InterruptStoredRecording(){++interrupts;}
};
struct Application {
 bool service=true;std::atomic<bool> orbit_service_recording_{true};
 std::shared_ptr<FakeRecorder> provisions_recorder_=std::make_shared<FakeRecorder>();
 std::shared_ptr<WebsocketProtocol> protocol=std::make_shared<WebsocketProtocol>();
 std::string orbit_service_status_;unsigned stopped=0,released=0;
 std::vector<std::function<void()>> main;
 bool IsOrbitService(){return service;}auto GetProtocol(){return protocol;}
 void StopListening(){++stopped;}void EndLocalRecordingOnMain(){++released;}
 void PaintOrbitService(){}void Schedule(std::function<void()> fn){main.push_back(std::move(fn));}
 void Drain(){auto work=std::move(main);main.clear();for(auto& fn:work)fn();}
 void OrbitServiceReviewFrame(const cJSON*,const std::string&){}
 void OrbitServiceFrame(const std::string& state,const std::string& detail=""){ __DISCARD__ }
 void Receive(const std::string& json) {
  std::unique_ptr<cJSON,decltype(&cJSON_Delete)> parsed(cJSON_Parse(json.c_str()),cJSON_Delete);
  auto* root=parsed.get();auto* type=cJSON_GetObjectItemCaseSensitive(root,"type");
  assert(cJSON_IsString(type)); __ROUTE__
 }
};
std::string frame(const std::string& id,const std::string& session="current-session") {
 return "{\"type\":\"dojo_service\",\"state\":\"discarded\",\"session_id\":\""+session+"\",\"recording_id\":\""+id+"\"}";
}
int main(){
 const std::string id="11111111-1111-4111-8111-111111111111";
 const std::string other="22222222-2222-4222-8222-222222222222";
 Application app;assert(ParseVoiceId(id.c_str(),app.provisions_recorder_->record.id));
 app.Receive(frame(id,"stale-session"));app.Receive(frame(other));app.Receive(frame("invalid"));
 assert(app.stopped==0&&app.main.empty()&&app.provisions_recorder_->requests==0);
 app.service=false;app.Receive(frame(id));assert(app.stopped==0);app.service=true;
 app.Receive(frame(id));
 assert(app.stopped==1&&!app.orbit_service_recording_&&app.released==0&&app.provisions_recorder_->requests==0);
 app.Drain();assert(app.released==1&&app.provisions_recorder_->requests==1&&app.protocol->interrupts==1);
 assert(app.orbit_service_status_=="Discarding at desk");
 // Repeated delivery is safe; completion is a separate worker result.
 app.Receive(frame(id));app.Drain();assert(app.provisions_recorder_->requests==2);
}
'''
        program = (program.replace("__HEX__", method(policy, "bool IsHexCharacter("))
                   .replace("__UUID__", method(policy, "bool IsCanonicalUuid("))
                   .replace("__DISCARD__", discard).replace("__ROUTE__", route))
        with tempfile.TemporaryDirectory(prefix="orbit-discard-frame-") as folder:
            path = Path(folder)
            for name, text in HEADERS.items():
                target = path / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            (path / "test.cc").write_text(program)
            cjson = ROOT / "managed_components/espressif__cjson/cJSON"
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(["cc", *flags, "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(path / "json.o")], check=True, capture_output=True)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                            "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson),
                            str(path / "test.cc"), str(ROOT / "main/provisions_voice_wire.cc"),
                            str(ROOT / "main/provisions_voice_recording.cc"), str(path / "json.o"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True, env={**os.environ, "ASAN_OPTIONS":
                            "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})
