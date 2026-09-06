#!/usr/bin/env python3
"""Verify the packaged S3 Opus decoder's complete 60 ms packet accounting in QEMU.

Uses an isolated generated project outside the checkout, no device or credentials.
Requires the canonical ESP-IDF Docker image and prepared codec dependencies.
"""
import argparse
import hashlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include "esp_opus_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
struct OpusEncoder;
OpusEncoder* opus_encoder_create(int,int,int,int*);
int opus_encode(OpusEncoder*,const int16_t*,int,unsigned char*,int);
void opus_encoder_destroy(OpusEncoder*);
}
static int16_t input[1440], output[1440];
static unsigned char packet[2048];
static void probe(void*) {
    uint32_t noise=321;
    for(int kind=0;kind<3;++kind) {
        int error=0;
        auto* encoder=opus_encoder_create(24000,1,2049,&error);
        assert(encoder && error==0);
        esp_opus_dec_cfg_t cfg{24000,1,ESP_OPUS_DEC_FRAME_DURATION_60_MS,false};
        void* decoder=nullptr;
        assert(esp_opus_dec_open(&cfg,sizeof(cfg),&decoder)==ESP_AUDIO_ERR_OK && decoder);
        for(int frame=0;frame<4;++frame) {
            for(int i=0;i<1440;++i) {
                noise=noise*1664525+1013904223;
                input[i]=kind==0 ? 0 : kind==1 ? int16_t(noise>>17) : int16_t(16000*std::sin(2*3.141592653589793*440*(i+frame*1440)/24000));
            }
            const int bytes=opus_encode(encoder,input,1440,packet,sizeof(packet));
            assert(bytes>0 && bytes<=2048);
            esp_audio_dec_in_raw_t raw{packet,unsigned(bytes),0,ESP_AUDIO_DEC_RECOVERY_NONE};
            esp_audio_dec_out_frame_t out{};out.buffer=reinterpret_cast<uint8_t*>(output);out.len=sizeof(output);
            esp_audio_dec_info_t info{};
            assert(esp_opus_dec_decode(decoder,&raw,&out,&info)==ESP_AUDIO_ERR_OK);
            assert(raw.consumed==unsigned(bytes) && out.decoded_size==2880);
            printf("ORBIT_TIMER_DECODE kind=%d frame=%d bytes=%d consumed=%u pcm_bytes=%u\n",kind,frame,bytes,unsigned(raw.consumed),unsigned(out.decoded_size));
        }
        esp_opus_dec_close(decoder);opus_encoder_destroy(encoder);
    }
    puts("ORBIT_TIMER_DECODER_PASS");fflush(stdout);vTaskDelete(nullptr);
}
extern "C" void app_main(){assert(xTaskCreate(probe,"timer_decode",40960,nullptr,3,nullptr)==pdPASS);}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    output = parser.parse_args().output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error("--output must be outside the firmware checkout")
    (output / "main").mkdir(parents=True, exist_ok=True)
    (output / "main/probe.cc").write_text(PROBE)
    (output / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS /workspace/managed_components/espressif__esp_audio_codec)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(orbit_timer_decoder_probe)
''')
    (output / "main/CMakeLists.txt").write_text('''idf_component_register(
    SRCS probe.cc INCLUDE_DIRS . REQUIRES espressif__esp_audio_codec)
''')
    (output / "sdkconfig.defaults").write_text('''CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_INT_WDT=n
''')
    command = ["docker", "run", "--rm", "--network", "none", "-v", f"{ROOT}:/workspace:ro",
               "-v", f"{output}:/probe", "-w", "/probe", "espressif/idf:v6.0.2", "bash", "-lc",
               "idf.py set-target esp32s3 && idf.py build && idf.py merge-bin -o flash.bin && "
               "truncate -s 4M build/flash.bin && timeout -k 2 10 qemu-system-xtensa "
               "-machine esp32s3 -nographic -drive file=build/flash.bin,if=mtd,format=raw"]
    log = output / "probe.log"
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
    evidence = log.read_text()
    if result.returncode not in (0, 124) or "ORBIT_TIMER_DECODER_PASS" not in evidence or any(
            fault in evidence for fault in ("panic'ed", "assert failed", "CORRUPT")):
        raise SystemExit(f"Timer decoder probe failed; inspect {log}")
    library = ROOT / "managed_components/espressif__esp_audio_codec/lib/esp32s3/libesp_audio_codec.a"
    print(f"library_sha256={hashlib.sha256(library.read_bytes()).hexdigest()}")
    for line in evidence.splitlines():
        if line.startswith("ORBIT_TIMER_"):
            print(line)
    print(f"Full log: {log}")


if __name__ == "__main__":
    main()
