"""Compile the real wire parser against the firmware's cJSON, with sanitizers."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    "freertos/FreeRTOS.h": "#pragma once\n",
    "freertos/task.h": "#pragma once\nusing TaskHandle_t=void*;\n",
    "esp_partition.h": "#pragma once\nstruct esp_partition_t;\n",
}

PROGRAM = r'''
#include "provisions_voice_wire.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
using namespace provisions;
VoiceReplay::~VoiceReplay() = default;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json parse(const std::string& text) {
    Json value(cJSON_Parse(text.c_str()),cJSON_Delete);assert(value);return value;
}
const std::string conversation="11111111-2222-4333-8444-555555555555";
const std::string source="22222222-3333-4444-8555-666666666666";
const std::string request="33333333-4444-4555-8666-777777777777";
std::string context(const std::string& from="null",const std::string& revision="null") {
    return "{\"conversation_id\":\""+conversation+"\",\"source_request_id\":"+from+
        ",\"source_revision\":"+revision+"}";
}
Json receipt(const std::string& status="transcribed",bool durable=true) {
    return parse("{\"session_id\":\"authenticated-session\",\"type\":\"capture_receipt\","
        "\"request_id\":\""+request+"\",\"capture\":{\"conversation_id\":\""+conversation+
        "\",\"source_request_id\":\""+source+"\",\"source_revision\":7,"
        "\"audio_sha256\":\""+std::string(64,'a')+"\",\"audio_bytes\":5,\"packet_count\":1,"
        "\"captured_unix_ms\":1788712345678},\"state\":\""+status+"\",\"durable\":"+
        (durable?"true":"false")+"}");
}
void replace(cJSON* object,const char* key,const std::string& value) {
    auto parsed=parse(value);assert(cJSON_ReplaceItemInObjectCaseSensitive(object,key,parsed.release()));
}
int main() {
    VoiceId id{};assert(ParseVoiceId(conversation.c_str(),id));assert(VoiceIdText(id)==conversation);
    for(const char* invalid:{"","00000000-0000-0000-0000-000000000000",
        "AAAAAAAA-bbbb-4ccc-8ddd-eeeeeeeeeeee","111111112222-4333-8444-555555555555",
        "11111111-2222-4333-8444-55555555555g","11111111-2222-4333-8444-555555555555 "})
        assert(!ParseVoiceId(invalid,id));
    assert(!ParseVoiceId(nullptr,id));
    VoiceContext ctx;
    auto empty=parse(context());assert(ParseVoiceContext(empty.get(),ctx));
    assert(ctx.source_request_id==VoiceId{} && ctx.source_revision==0);
    for(const std::string revision:{"0","1","999999999"}) {
        auto value=parse(context("\""+source+"\"",revision));assert(ParseVoiceContext(value.get(),ctx));
    }
    for(const std::string revision:{"null","true","[]","{}","\"1\"","-1","0.5","1000000000","1e309"}) {
        auto value=parse(context("\""+source+"\"",revision));assert(!ParseVoiceContext(value.get(),ctx));
    }
    for(const std::string revision:{"0","1","false"}) {
        auto value=parse(context("null",revision));assert(!ParseVoiceContext(value.get(),ctx));
    }
    for(const char* invalid:{"[]","null","{}"}) {auto value=parse(invalid);assert(!ParseVoiceContext(value.get(),ctx));}
    auto duplicate=parse(context());cJSON_AddNullToObject(duplicate.get(),"source_revision");
    assert(!ParseVoiceContext(duplicate.get(),ctx));
    auto extra=parse(context());cJSON_AddNullToObject(extra.get(),"command");assert(!ParseVoiceContext(extra.get(),ctx));
    VoiceCaptureReceipt out;
    for(const std::string status:{"processing","pending","needs_attention","transcribed","unintelligible"}) {
        const bool terminal=status=="transcribed"||status=="unintelligible";
        auto value=receipt(status,terminal);assert(ParseVoiceReceipt(value.get(),out));
        assert(out.durable==terminal && out.needs_attention==(status=="needs_attention"));
        auto wrong=receipt(status,!terminal);assert(!ParseVoiceReceipt(wrong.get(),out));
    }
    for(const char* field:{"type","request_id","capture","state","durable"})
        for(const std::string invalid:{"null","[]","{}","42"}) {
            auto value=receipt();replace(value.get(),field,invalid);assert(!ParseVoiceReceipt(value.get(),out));
        }
    for(const std::string status:{"saved","answered","deferred","","TRANSCRIBED"}) {
        auto value=receipt(status);assert(!ParseVoiceReceipt(value.get(),out));
    }
    for(const char* field:{"audio_sha256","audio_bytes","packet_count","captured_unix_ms","source_revision"}) {
        for(const std::string invalid:{"null","[]","{}","true","\"1\"","-1","1.5","1e309"}) {
            auto value=receipt();auto cap=cJSON_GetObjectItemCaseSensitive(value.get(),"capture");
            replace(cap,field,invalid);assert(!ParseVoiceReceipt(value.get(),out));
        }
    }
    for(const std::string invalid:{"0","168","4294967296"}) {
        auto value=receipt();replace(cJSON_GetObjectItemCaseSensitive(value.get(),"capture"),"packet_count",invalid);
        assert(!ParseVoiceReceipt(value.get(),out));
    }
    for(const std::string invalid:{"2","2051","342351"}) {
        auto value=receipt();replace(cJSON_GetObjectItemCaseSensitive(value.get(),"capture"),"audio_bytes",invalid);
        assert(!ParseVoiceReceipt(value.get(),out));
    }
    for(const std::string& invalid:{"\""+std::string(64,'A')+"\"","\""+std::string(63,'a')+"\""}) {
        auto value=receipt();replace(cJSON_GetObjectItemCaseSensitive(value.get(),"capture"),"audio_sha256",invalid);
        assert(!ParseVoiceReceipt(value.get(),out));
    }
    auto maximum=receipt();auto cap=cJSON_GetObjectItemCaseSensitive(maximum.get(),"capture");
    replace(cap,"packet_count","167");replace(cap,"audio_bytes","342350");
    replace(cap,"captured_unix_ms","253402300799999");assert(ParseVoiceReceipt(maximum.get(),out));
    replace(cap,"captured_unix_ms","253402300800000");assert(!ParseVoiceReceipt(maximum.get(),out));
    for(bool nested:{false,true}) {
        auto value=receipt();auto object=nested?cJSON_GetObjectItemCaseSensitive(value.get(),"capture"):value.get();
        cJSON_AddNullToObject(object,nested?"source_revision":"state");assert(!ParseVoiceReceipt(value.get(),out));
    }
    // The caller separately authenticates session_id against the live socket.
    // The builder must preserve arbitrary session text without introducing fields.
    auto valid=receipt();assert(ParseVoiceReceipt(valid.get(),out));
    VoiceReplay replay;replay.capture=out.capture;replay.bytes=out.bytes;replay.digest=out.digest;
    const std::string session="quote\"\\\n,\"durable\":true";
    auto start=parse(VoiceCaptureStart(replay,session,2147483647,true));
    assert(cJSON_GetArraySize(start.get())==8);
    assert(session==cJSON_GetObjectItemCaseSensitive(start.get(),"session_id")->valuestring);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(start.get(),"deferred")));
    assert(cJSON_GetObjectItemCaseSensitive(start.get(),"turn_id")->valuedouble==2147483647);
    assert(request==cJSON_GetObjectItemCaseSensitive(start.get(),"request_id")->valuestring);
    assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(start.get(),"capture"),
                         cJSON_GetObjectItemCaseSensitive(valid.get(),"capture"),true));
    replay.capture.source_request_id={};replay.capture.source_revision=0;
    start=parse(VoiceCaptureStart(replay,session,1,false));cap=cJSON_GetObjectItemCaseSensitive(start.get(),"capture");
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cap,"source_request_id")));
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cap,"source_revision")));
    std::cout<<"Strict wire context, receipt state, bounds and serialization passed\n";
}
'''


class VoiceWireReviewTests(unittest.TestCase):
    def test_actual_wire_parser_and_serializer(self):
        compiler = shutil.which("c++")
        ccompiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        self.assertIsNotNone(ccompiler)
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        if not (cjson / "cJSON.c").exists():
            self.skipTest("Run the canonical firmware dependency preparation to supply cJSON")
        with tempfile.TemporaryDirectory(prefix="orbit-wire-review-") as folder:
            path = Path(folder)
            for name, source in HEADERS.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(source)
            (path / "review.cc").write_text(PROGRAM)
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([ccompiler, *sanitize, "-Wno-deprecated-declarations", "-I", str(cjson), "-c", str(cjson / "cJSON.c"),
                            "-o", str(path / "cjson.o")], check=True)
            binary = path / "review"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *sanitize,
                            "-I", str(path), "-I", str(ROOT / "main"), "-I", str(cjson),
                            str(path / "review.cc"), str(ROOT / "main/provisions_voice_wire.cc"),
                            str(ROOT / "main/provisions_voice_recording.cc"), str(path / "cjson.o"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS":
                "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
