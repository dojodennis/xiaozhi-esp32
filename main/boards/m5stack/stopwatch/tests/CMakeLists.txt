cmake_minimum_required(VERSION 3.20)
project(orbit_service_schedule_host LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT IS_DIRECTORY "${ORBIT_MANAGED_COMPONENTS}/lvgl__lvgl")
  message(FATAL_ERROR "Set ORBIT_MANAGED_COMPONENTS to an existing firmware managed_components directory")
endif()
get_filename_component(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(MAIN_DIR "${BOARD_DIR}/../../.." ABSOLUTE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
add_subdirectory("${ORBIT_MANAGED_COMPONENTS}/lvgl__lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdkconfig.h"
  "#pragma once\n#define CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO 1\n#define CONFIG_PROVISIONS_GATEWAY_REQUIRED 1\n")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/service_schedule_demo_fixture.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_demo_fixture.py"
          "${CMAKE_CURRENT_BINARY_DIR}/service_schedule_demo_fixture.h"
  DEPENDS generate_demo_fixture.py orbit_service_schedule_v1.json VERBATIM)
set(CJSON_DIR "${ORBIT_MANAGED_COMPONENTS}/espressif__cjson/cJSON")
set(FONT_DIR "${ORBIT_MANAGED_COMPONENTS}/78__xiaozhi-fonts/src")
add_executable(service_schedule_demo_host
  service_schedule_demo_host.cc
  "${BOARD_DIR}/service_schedule.cc" "${BOARD_DIR}/service_schedule_wire.cc"
  "${BOARD_DIR}/service_schedule_face.cc" "${BOARD_DIR}/service_schedule_view.cc"
  "${BOARD_DIR}/service_schedule_demo.cc" "${BOARD_DIR}/orbit_dial.cc"
  "${CJSON_DIR}/cJSON.c"
  "${FONT_DIR}/font_noto_sans_basic_16_4.c" "${FONT_DIR}/font_noto_sans_basic_30_4.c"
  "${CMAKE_CURRENT_BINARY_DIR}/service_schedule_demo_fixture.h")
target_include_directories(service_schedule_demo_host PRIVATE
  "${BOARD_DIR}" "${MAIN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${CJSON_DIR}")
target_link_libraries(service_schedule_demo_host PRIVATE lvgl)
target_compile_options(service_schedule_demo_host PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-pedantic>")
