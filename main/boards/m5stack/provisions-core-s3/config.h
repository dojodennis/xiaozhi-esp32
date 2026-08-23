#ifndef _PROVISIONS_CORE_S3_CONFIG_H_
#define _PROVISIONS_CORE_S3_CONFIG_H_

#include <driver/gpio.h>

// CoreS3 and CoreS3 Lite use the same internal audio, display, PMIC and IO
// expander wiring. Keep only the external/base differences profile-guarded.
#if defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3) && \
    defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE)
#error "Select exactly one Provisions CoreS3 hardware profile"
#elif defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3_LITE)
#define PROVISIONS_HARDWARE_PROFILE "core-s3-lite-client"
#define PROVISIONS_NOMINAL_BATTERY_MAH 200
#define PROVISIONS_HAS_DIN_BASE false
// U027 on the Lite's exposed Port.A: black=GND, red=5V, white=GPIO1.
#define TALK_BUTTON_GPIO GPIO_NUM_1
#elif defined(CONFIG_BOARD_TYPE_M5STACK_PROVISIONS_CORE_S3)
#define PROVISIONS_HARDWARE_PROFILE "core-s3-bench"
#define PROVISIONS_NOMINAL_BATTERY_MAH 500
#define PROVISIONS_HAS_DIN_BASE true
// U027 on the full CoreS3 DIN base Port.B: black=GND, red=5V, white=GPIO8.
#define TALK_BUTTON_GPIO GPIO_NUM_8
#else
#error "A Provisions CoreS3 hardware profile is required"
#endif

// Shared internal hardware.
#define AUDIO_INPUT_REFERENCE false
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_0
#define AUDIO_I2S_GPIO_WS GPIO_NUM_33
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_34
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_14
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13

#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_12
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_11
#define AUDIO_CODEC_AW88298_ADDR AW88298_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR ES7210_CODEC_DEFAULT_ADDR

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

#endif  // _PROVISIONS_CORE_S3_CONFIG_H_
