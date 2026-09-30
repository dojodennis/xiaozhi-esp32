cmake_minimum_required(VERSION 3.16)
project(orbit_menu_runtime LANGUAGES C CXX)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
set(BOARD "${ROOT}/main/boards/m5stack/stopwatch")
set(FONTS "${ROOT}/managed_components/78__xiaozhi-fonts/src")
add_executable(orbit_menu_runtime runtime.cc "${BOARD}/menu_crest.cc"
    "${BOARD}/crest_asset.cc" "${BOARD}/menu_asset.cc"
    "${FONTS}/font_noto_sans_basic_16_4.c"
    "${FONTS}/font_noto_sans_basic_30_4.c")
target_compile_features(orbit_menu_runtime PRIVATE cxx_std_17)
target_include_directories(orbit_menu_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${BOARD}")
target_link_libraries(orbit_menu_runtime PRIVATE lvgl)
