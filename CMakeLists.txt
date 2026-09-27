# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.4.8")
project(xiaozhi)

# LVGL and esp_emote_gfx ship different qrcodegen copies with the same public
# C symbols. Keep emote's API private to its library when LVGL QR is enabled.
# Renaming all emote callers with the implementation avoids link-order selection
# of an incompatible encoder and leaves managed source files untouched.
if(CONFIG_LV_USE_QRCODE AND TARGET __idf_espressif2022__esp_emote_gfx)
    foreach(symbol encodeText encodeBinary encodeSegments encodeSegmentsAdvanced
                   getSize getModule isAlphanumeric isNumeric calcSegmentBufferSize
                   makeBytes makeNumeric makeAlphanumeric makeEci)
        target_compile_definitions(__idf_espressif2022__esp_emote_gfx PRIVATE
            "qrcodegen_${symbol}=emote_qrcodegen_${symbol}")
    endforeach()
endif()
