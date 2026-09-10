#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

// Board adapter for MeshCore's ESP32Board. The helper is only compiled so
// MeshCore helpers compile (the instance is never used by this app).
// Generic boards default to lib/GenericSX1262Board.h; Heltec envs override
// with their variant header (variants/heltec_wifi_lora_32_V4/HeltecV4Board.h).
// Board adapter for MeshCore's ESP32Board. The helper is only compiled so
// MeshCore helpers compile (the instance is never used by this app).
// Generic boards default to lib/GenericSX1262Board.h; Heltec envs set
// -DHELTEC_V4_BOARD and use the variant header
// (variants/heltec_wifi_lora_32_V4/HeltecV4Board.h).
#ifdef HELTEC_V4_BOARD
  #define TARGET_BOARD_CLASS HeltecV4Board
  #include <HeltecV4Board.h>
#else
  #define TARGET_BOARD_CLASS GenericSX1262Board
  #include <GenericSX1262Board.h>
#endif

// Generic SX1262 board: RADIO_CLASS/WRAPPER_CLASS may be overridden per build.
#ifndef RADIO_CLASS
  #define RADIO_CLASS CustomSX1262
#endif
#ifndef WRAPPER_CLASS
  #define WRAPPER_CLASS CustomSX1262Wrapper
#endif

#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#ifdef DISPLAY_CLASS
#ifdef HELTEC_LORA_V4_OLED
    #include <helpers/ui/SSD1306Display.h>
#elif defined(HELTEC_LORA_V4_TFT)
    #include <helpers/ui/ST7789LCDDisplay.h>
#endif
  #include <helpers/ui/MomentaryButton.h>
#endif

extern TARGET_BOARD_CLASS board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();

