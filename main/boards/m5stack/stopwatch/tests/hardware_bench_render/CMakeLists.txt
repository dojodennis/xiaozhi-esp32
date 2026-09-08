cmake_minimum_required(VERSION 3.20)
project(orbit_hardware_bench_render LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(BOARD_DIR "${TEST_DIR}/.." ABSOLUTE)
get_filename_component(MAIN_DIR "${BOARD_DIR}/../../.." ABSOLUTE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${TEST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
add_subdirectory("${ORBIT_MANAGED_COMPONENTS}/lvgl__lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdkconfig.h"
  "#pragma once\n#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n#define CONFIG_PROVISIONS_SCHEDULE_HARDWARE_BENCH 1\n#define CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO 0\n")
set(CJSON_DIR "${ORBIT_MANAGED_COMPONENTS}/espressif__cjson/cJSON")
set(FONT_DIR "${ORBIT_MANAGED_COMPONENTS}/78__xiaozhi-fonts/src")
add_executable(hardware_bench_render host.cc
  "${BOARD_DIR}/service_schedule.cc" "${BOARD_DIR}/service_schedule_face.cc"
  "${BOARD_DIR}/service_schedule_worker.cc" "${BOARD_DIR}/service_schedule_storage.cc"
  "${BOARD_DIR}/service_schedule_hardware_bench.cc" "${BOARD_DIR}/service_schedule_alarm_output.cc"
  "${BOARD_DIR}/orbit_dial.cc" "${CJSON_DIR}/cJSON.c"
  "${FONT_DIR}/font_noto_sans_basic_16_4.c" "${FONT_DIR}/font_noto_sans_basic_30_4.c")
target_include_directories(hardware_bench_render PRIVATE
  "${BOARD_DIR}" "${MAIN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${CJSON_DIR}")
target_link_libraries(hardware_bench_render PRIVATE lvgl)
target_compile_options(hardware_bench_render PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-pedantic>")
