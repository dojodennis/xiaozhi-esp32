#!/usr/bin/env python3
"""Run actual Talk callbacks on ESP_TIMER_TASK's configured 3584-byte stack.

Uses ESP-IDF 6.0.2 in QEMU, real event bits, actual Button wrappers and physical
Application/AudioService methods. Driver registration is a test seam; no GPIO,
USB, room audio, network or device state is accessed. Heavy startup is tested
separately by the host release/new-press reentrancy test.
"""
import argparse
import re
import subprocess
from pathlib import Path
from verify_provisions_opus_tail import method

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "provisions_reply_turn.h"
#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1
#define CONFIG_PROVISIONS_LOCAL_CAPTURE 1
constexpr int MAIN_EVENT_START_LISTENING=1,MAIN_EVENT_STOP_LISTENING=2;
constexpr int BUTTON_PRESS_DOWN=0,BUTTON_PRESS_UP=1;
struct Registration{void(*callback)(void*,void*)=nullptr;void* context=nullptr;}registered[2];
void iot_button_register_cb(void*,int event,void*,void(*callback)(void*,void*),void* context){registered[event]={callback,context};}
struct Button{void* button_handle_=this;std::function<void()> on_press_down_,on_press_up_;void OnPressDown(std::function<void()>);void OnPressUp(std::function<void()>);};
struct AudioService{
 std::atomic<uint32_t> local_physical_boundary_{0},local_recording_press_{0};
 void FenceLocalRecording(uint32_t);void ReleaseLocalRecordingFence(uint32_t);
};
struct Application{
 ProvisionsReplyTurn provisions_physical_press_;
 std::atomic<bool> manual_listening_requested_{false};
 AudioService audio_service_;EventGroupHandle_t event_group_=nullptr;
 std::mutex provisions_recording_control_mutex_;
 static Application& GetInstance(){static Application app;return app;}
 void StartListening();void StopListening();
};
__METHODS__
static Button button1_;
static std::atomic<unsigned> cycles{0},minimum_stack{UINT32_MAX};
static void timer_callback(void*) {
 assert(std::strcmp(pcTaskGetName(nullptr),"esp_timer")==0);
 auto& app=Application::GetInstance();
 unsigned current=cycles.load()+1;
 registered[BUTTON_PRESS_DOWN].callback(nullptr,registered[BUTTON_PRESS_DOWN].context);
 assert(app.manual_listening_requested_.load());
 assert(app.provisions_physical_press_.id()==current);
 assert(app.audio_service_.local_physical_boundary_==current);
 assert(app.audio_service_.local_recording_press_==0);
 registered[BUTTON_PRESS_UP].callback(nullptr,registered[BUTTON_PRESS_UP].context);
 assert(!app.manual_listening_requested_.load());
 assert(app.audio_service_.local_physical_boundary_==(current|0x80000000U));
 assert((xEventGroupGetBits(app.event_group_)&3)==3);
 xEventGroupClearBits(app.event_group_,3);
 unsigned remaining=uxTaskGetStackHighWaterMark(nullptr);
 if(remaining<minimum_stack.load())minimum_stack=remaining;
 cycles=current;
}
extern "C" void app_main(){
 static_assert(CONFIG_ESP_TIMER_TASK_STACK_SIZE==3584);
 static_assert(std::atomic<uint32_t>::is_always_lock_free);
 static_assert(std::atomic<bool>::is_always_lock_free);
 auto& app=Application::GetInstance();
 app.event_group_=xEventGroupCreate();assert(app.event_group_);
 __BOARD_CALLBACKS__
 esp_timer_handle_t timer;
 esp_timer_create_args_t args{};args.callback=timer_callback;args.dispatch_method=ESP_TIMER_TASK;args.name="talk_probe";
 assert(esp_timer_create(&args,&timer)==ESP_OK);
 // If a button ever takes the recording control lock again, callbacks stall
 // here and the probe fails its deadline while the main task holds that lock.
 std::lock_guard<std::mutex> main_busy(app.provisions_recording_control_mutex_);
 assert(esp_timer_start_periodic(timer,1000)==ESP_OK);
 const auto deadline=esp_timer_get_time()+3000000;
 while(cycles<1000 && esp_timer_get_time()<deadline)vTaskDelay(1);
 assert(esp_timer_stop(timer)==ESP_OK);assert(cycles>=1000);
 assert(minimum_stack>=1024);
 printf("ORBIT_BUTTON_TIMER_PASS cycles=%u stack_bytes=%d minimum_free=%u\n",cycles.load(),CONFIG_ESP_TIMER_TASK_STACK_SIZE,minimum_stack.load());fflush(stdout);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    output = parser.parse_args().output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error("--output must be outside the checkout")
    (output / "main").mkdir(parents=True, exist_ok=True)
    specs = [("main/boards/common/button.cc", f"void Button::{name}(") for name in ("OnPressDown", "OnPressUp")]
    specs += [("main/application.cc", f"void Application::{name}()") for name in ("StartListening", "StopListening")]
    specs += [("main/audio/audio_service.cc", f"void AudioService::{name}(") for name in ("FenceLocalRecording", "ReleaseLocalRecordingFence")]
    methods = "\n".join(method((ROOT / path).read_text(), signature) for path, signature in specs)
    board = (ROOT / "main/boards/m5stack/stopwatch/m5stack_stopwatch.cc").read_text()
    callbacks = "\n".join(re.search(r"button1_\." + name + r"\(\[.*?\}\);", board, re.S)[0]
                          for name in ("OnPressDown", "OnPressUp"))
    (output / "main/probe.cc").write_text(PROBE.replace("__METHODS__", methods).replace("__BOARD_CALLBACKS__", callbacks))
    (output / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(orbit_button_probe)
''')
    (output / "main/CMakeLists.txt").write_text('''idf_component_register(SRCS probe.cc INCLUDE_DIRS /workspace/main REQUIRES esp_timer)
''')
    (output / "sdkconfig.defaults").write_text('''CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_INT_WDT=n
''')
    command = ["docker", "run", "--rm", "--network", "none", "-v", f"{ROOT}:/workspace:ro",
               "-v", f"{output}:/probe", "-w", "/probe", "espressif/idf:v6.0.2", "bash", "-lc",
               "idf.py set-target esp32s3 && idf.py build && idf.py merge-bin -o flash.bin && "
               "truncate -s 4M build/flash.bin && timeout -k 2 10 qemu-system-xtensa -machine esp32s3 "
               "-nographic -drive file=build/flash.bin,if=mtd,format=raw"]
    log = output / "probe.log"
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
    evidence = log.read_text()
    if result.returncode not in (0, 124) or "ORBIT_BUTTON_TIMER_PASS" not in evidence or any(
            fault in evidence for fault in ("panic'ed", "assert failed", "CORRUPT")):
        raise SystemExit(f"Button timer probe failed; inspect {log}")
    print(next(line for line in evidence.splitlines() if line.startswith("ORBIT_BUTTON_TIMER_PASS")))
    print(f"Full log: {log}")


if __name__ == "__main__":
    main()
