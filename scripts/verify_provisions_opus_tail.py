#!/usr/bin/env python3
"""Run the actual recorder Encode method with the packaged S3 codec in QEMU.

Needs Docker's espressif/idf:v6.0.2 image and the configured managed components.
No device, network, audio output or persistent credentials are used. Generated
files and logs stay in --output, which must be outside the firmware checkout.
"""
import argparse
import hashlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


PROBE = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mbedtls/platform_util.h>
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisions_voice_recording.h"
extern "C" {
struct OpusEncoder;
struct OpusDecoder;
OpusEncoder* __real_opus_encoder_create(int32_t,int,int,int*);
void __real_opus_encoder_destroy(OpusEncoder*);
int opus_encoder_ctl(OpusEncoder*,int,...);
OpusDecoder* opus_decoder_create(int32_t,int,int*);
int opus_decode(OpusDecoder*,const unsigned char*,int32_t,int16_t*,int,int);
void opus_decoder_destroy(OpusDecoder*);
}
static int latest_lookahead=-1;
extern "C" OpusEncoder* __wrap_opus_encoder_create(int32_t rate,int channels,int mode,int* error) {
    assert(rate==16000 && channels==1);
    return __real_opus_encoder_create(rate,channels,mode,error);
}
extern "C" void __wrap_opus_encoder_destroy(OpusEncoder* encoder) {
    // Query the actual encoder after the ESP wrapper configured/used it. The
    // public CTL is used only by this probe, never by production wrapper casts.
    assert(opus_encoder_ctl(encoder,4027,&latest_lookahead)==0);
    assert(latest_lookahead>0 && latest_lookahead<=320);
    __real_opus_encoder_destroy(encoder);
}
namespace provisions {
__CONSTANTS__
class VoiceRecorder { public:
    uint8_t* frames_;
    bool Encode(const VoiceRecording::Work&, VoiceCapture&, size_t&);
};
__ENCODE__
}
static int16_t input[16000];
static uint8_t frames[32768];
static int16_t decoded[960];
static void probe(void*) {
    provisions::VoiceRecorder recorder{frames};
    provisions::VoiceRecording::Work work;
    work.pcm=input;
    uint32_t noise=123;
    for(int kind=0;kind<3;++kind) {
        for(size_t samples: {size_t(160),size_t(960),size_t(1920),size_t(16000)}) {
            for(size_t i=0;i<samples;i++) {
                noise=noise*1664525+1013904223;
                input[i]=kind==0 ? 0 : kind==1 ? int16_t(noise>>17) :
                    int16_t(16000*std::sin(2*3.141592653589793*440*i/16000));
            }
            work.samples=samples;
            provisions::VoiceCapture capture;
            size_t bytes=0;
            assert(recorder.Encode(work,capture,bytes));
            assert(capture.packet_count==(samples+320+959)/960);
            assert(bytes<sizeof(frames));
            const auto free_stack=uxTaskGetStackHighWaterMark(nullptr);
            assert(free_stack>=8192);
            printf("ORBIT_ENCODE kind=%d samples=%u packets=%u lookahead=%d stack_free=%u\n",
                kind,unsigned(samples),unsigned(capture.packet_count),latest_lookahead,unsigned(free_stack));
            if(kind==2 && samples==960) {
                int error=0;
                auto* decoder=opus_decoder_create(16000,1,&error);
                assert(decoder && error==0);
                size_t offset=0;long long tail_energy=0;
                for(unsigned packet=0;packet<capture.packet_count;packet++) {
                    size_t len=frames[offset] | (frames[offset+1]<<8);offset+=2;
                    assert(offset+len<=bytes);
                    assert(opus_decode(decoder,frames+offset,len,decoded,960,0)==960);
                    offset+=len;
                    if(packet==1)
                        for(int i=0;i<latest_lookahead;i++)
                            tail_energy+=static_cast<long long>(decoded[i])*decoded[i];
                }
                assert(offset==bytes && tail_energy>1000000);
                printf("ORBIT_TAIL_RECOVERED energy=%lld\n",tail_energy);
                opus_decoder_destroy(decoder);
            }
            vTaskDelay(1);
        }
    }
    puts("ORBIT_OPUS_PROBE_PASS");fflush(stdout);vTaskDelete(nullptr);
}
extern "C" void app_main() {
    assert(xTaskCreate(probe,"record_probe",provisions::kRecordingStackBytes,nullptr,3,nullptr)==pdPASS);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error("--output must be outside the firmware checkout")
    output.mkdir(parents=True, exist_ok=True)
    (output / "main").mkdir(exist_ok=True)
    source = (ROOT / "main/provisions_voice_recorder.cc").read_text()
    constants = "\n".join(re.findall(
        r"constexpr size_t k(?:CaptureTailSamples|RecordingStackBytes) = [^;]+;", source))
    program = PROBE.replace("__CONSTANTS__", constants).replace(
        "__ENCODE__", method(source, "bool VoiceRecorder::Encode("))
    (output / "main/probe.cc").write_text(program)
    (output / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS /workspace/managed_components/espressif__esp_audio_codec)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(orbit_opus_probe)
''')
    (output / "main/CMakeLists.txt").write_text('''idf_component_register(
    SRCS probe.cc INCLUDE_DIRS /workspace/main REQUIRES espressif__esp_audio_codec mbedtls)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=opus_encoder_create" "-Wl,--wrap=opus_encoder_destroy")
''')
    (output / "sdkconfig.defaults").write_text('''CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_INT_WDT=n
''')
    log = output / "probe.log"
    command = ["docker", "run", "--rm", "--network", "none",
               "-v", f"{ROOT}:/workspace:ro", "-v", f"{output}:/probe", "-w", "/probe",
               "espressif/idf:v6.0.2", "bash", "-lc",
               "idf.py set-target esp32s3 && idf.py build && "
               "idf.py merge-bin -o flash.bin && truncate -s 4M build/flash.bin && "
               "timeout -k 2 10 qemu-system-xtensa -machine esp32s3 -nographic "
               "-drive file=build/flash.bin,if=mtd,format=raw"]
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
    evidence = log.read_text()
    if result.returncode not in (0,124) or "ORBIT_OPUS_PROBE_PASS" not in evidence or any(
            fault in evidence for fault in ("panic'ed", "assert failed", "CORRUPT")):
        raise SystemExit(f"Codec probe failed; inspect {log}")
    library = ROOT / "managed_components/espressif__esp_audio_codec/lib/esp32s3/libesp_audio_codec.a"
    print(f"library_sha256={hashlib.sha256(library.read_bytes()).hexdigest()}")
    for line in evidence.splitlines():
        if line.startswith("ORBIT_"):
            print(line)
    print(f"Full log: {log}")


if __name__ == "__main__":
    main()
