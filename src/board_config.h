#pragma once

// ============================================================================
// BOARD CONFIGURATION (multi-board support)
//
// All per-board differences live in platformio.ini build_flags and are mapped
// here onto the app-level macros used by main.cpp. Sensible defaults are the
// Heltec WiFi LoRa 32 V4.3 so a bare `pio run` just works.
//
// Per-board switches:
//   -DHAS_OLED=0        board has no display (serial-only build)
//   -DHAS_FEM=0         board has no external FEM/LNA module
//   -DOLED_DRIVER_SH1106=0   use Adafruit_SSD1306 instead of built-in SH1106 driver
//
// Pin flags (all optional, defaults = Heltec V4.3):
//   -DP_LORA_NSS/-DP_LORA_DIO_1/-DP_LORA_RESET/-DP_LORA_BUSY   SX1262 SPI
//   -DP_LORA_SCLK/-DP_LORA_MISO/-DP_LORA_MOSI                  SPI bus
//   -DPIN_BOARD_SDA/-DPIN_BOARD_SCL                            I2C
//   -DPIN_OLED_RESET                                           OLED reset GPIO
//   -DPIN_USER_BTN                                             user button (-1 = none)
//   -DPIN_VEXT_EN + -DPIN_VEXT_EN_ACTIVE                       peripheral power rail
//   -DP_LORA_PA_POWER/-DP_LORA_KCT8103L_PA_CSD/-DP_LORA_KCT8103L_PA_CTX  FEM pins
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// SX1262 SPI pins
// ---------------------------------------------------------------------------
#ifndef LORA_CS
  #if defined(P_LORA_NSS)
    #define LORA_CS   P_LORA_NSS
  #else
    #define LORA_CS   8
  #endif
#endif

#ifndef LORA_DIO1
  #if defined(P_LORA_DIO_1)
    #define LORA_DIO1 P_LORA_DIO_1
  #else
    #define LORA_DIO1 14
  #endif
#endif

#ifndef LORA_RST
  #if defined(P_LORA_RESET)
    #define LORA_RST  P_LORA_RESET
  #else
    #define LORA_RST  12
  #endif
#endif

#ifndef LORA_BUSY
  #if defined(P_LORA_BUSY)
    #define LORA_BUSY P_LORA_BUSY
  #else
    #define LORA_BUSY 13
  #endif
#endif

#ifndef LORA_SCK
  #if defined(P_LORA_SCLK)
    #define LORA_SCK  P_LORA_SCLK
  #else
    #define LORA_SCK  9
  #endif
#endif

#ifndef LORA_MISO
  #if defined(P_LORA_MISO)
    #define LORA_MISO P_LORA_MISO
  #else
    #define LORA_MISO 11
  #endif
#endif

#ifndef LORA_MOSI
  #if defined(P_LORA_MOSI)
    #define LORA_MOSI P_LORA_MOSI
  #else
    #define LORA_MOSI 10
  #endif
#endif

// ---------------------------------------------------------------------------
// Radio parameters
// ---------------------------------------------------------------------------
#ifndef LORA_FREQ
  #define LORA_FREQ      868.731018
#endif
#ifndef LORA_BW
  #define LORA_BW        62.5
#endif
#ifndef LORA_SF
  #define LORA_SF        8
#endif
#ifndef LORA_CR
  #define LORA_CR        7
#endif
#ifndef LORA_SYNC_WORD
  #define LORA_SYNC_WORD 0x12
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER  10
#endif
#ifndef LORA_PREAMBLE
  #define LORA_PREAMBLE  16
#endif

// ---------------------------------------------------------------------------
// FEM (external power amplifier / LNA board)
// ---------------------------------------------------------------------------
#ifdef P_LORA_PA_POWER
  #define FEM_VCC_PIN P_LORA_PA_POWER
#endif
#ifdef P_LORA_KCT8103L_PA_CSD
  #define FEM_EN_PIN  P_LORA_KCT8103L_PA_CSD
#endif
#ifdef P_LORA_KCT8103L_PA_CTX
  #define FEM_TX_PIN  P_LORA_KCT8103L_PA_CTX
#endif

#ifndef HAS_FEM
  #if defined(FEM_TX_PIN) && defined(FEM_EN_PIN) && defined(FEM_VCC_PIN)
    #define HAS_FEM 1
  #else
    #define HAS_FEM 0
    #warning "No FEM pins defined, building WITHOUT external FEM control"
  #endif
#endif

// ---------------------------------------------------------------------------
// Peripheral power rail (VEXT)
// ---------------------------------------------------------------------------
#ifdef PIN_VEXT_EN
  #define VEXT_PIN PIN_VEXT_EN
#endif
#ifndef VEXT_EN_ACTIVE
  #if defined(PIN_VEXT_EN_ACTIVE)
    #define VEXT_EN_ACTIVE PIN_VEXT_EN_ACTIVE
  #else
    #define VEXT_EN_ACTIVE LOW
  #endif
#endif

// ---------------------------------------------------------------------------
// User button
// ---------------------------------------------------------------------------
#ifndef BUTTON_PIN
  #if defined(PIN_USER_BTN)
    #define BUTTON_PIN PIN_USER_BTN
  #else
    #define BUTTON_PIN -1
  #endif
#endif

// ---------------------------------------------------------------------------
// I2C pins
// ---------------------------------------------------------------------------
#ifdef PIN_BOARD_SDA
  #define SDA_PIN PIN_BOARD_SDA
#endif
#ifdef PIN_BOARD_SCL
  #define SCL_PIN PIN_BOARD_SCL
#endif

// ---------------------------------------------------------------------------
// Display. HAS_OLED=0 selects a no-op stub so all main.cpp call sites stay as
// they are; it simply does nothing on boards without a screen.
// ---------------------------------------------------------------------------
#ifndef SCREEN_WIDTH
  #define SCREEN_WIDTH 128
#endif
#ifndef SCREEN_HEIGHT
  #define SCREEN_HEIGHT 64
#endif
#ifndef OLED_RESET
  #ifdef PIN_OLED_RESET
    #define OLED_RESET PIN_OLED_RESET
  #else
    #define OLED_RESET 21
  #endif
#endif
#ifndef SCREEN_ADDRESS
  #define SCREEN_ADDRESS 0x3C
#endif

#ifndef HAS_OLED
  #define HAS_OLED 1
#endif

#include <Adafruit_GFX.h>
#include "cyrillic.h"

#if HAS_OLED

  #ifndef OLED_DRIVER_SH1106
    #define OLED_DRIVER_SH1106 1
  #endif
  #if OLED_DRIVER_SH1106
    #include "sysoled.h"          // own compact SH1106 driver (Heltec V4 panels)
  #else
    #include <Adafruit_SSD1306.h>
  #endif

#else // no display -> stub with the same API

  class StubDisplay : public Adafruit_GFX {
  public:
    StubDisplay(int16_t w, int16_t h) : Adafruit_GFX(w, h) {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
      (void)x; (void)y; (void)color;
    }
    bool begin(uint8_t vcc, uint8_t addr) { (void)vcc; (void)addr; return true; }
    void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void clearDisplay(void) {}
    void display(void) {}
    void dim(bool) {}
    void setPower(bool) {}
  };

#endif // HAS_OLED

// Some draw calls in main.cpp use these SSD1306 constants; define them for the
// SH1106/stub paths where the Adafruit_SSD1306 header is not included.
#ifndef SSD1306_WHITE
  #define SSD1306_WHITE 1
#endif
#ifndef SSD1306_SWITCHCAPVCC
  #define SSD1306_SWITCHCAPVCC 1
#endif

// Instantiate the display object selected above.
#if HAS_OLED
  #if OLED_DRIVER_SH1106
    SysOled display(SCREEN_WIDTH, SCREEN_HEIGHT);
  #else
    // Same UTF-8 -> CP866 hook, applied to the Adafruit SSD1306 driver.
    class RusSSD1306 : public Adafruit_SSD1306 {
    public:
      using Adafruit_SSD1306::Adafruit_SSD1306;
      size_t write(uint8_t c) override {
        return utf8cp866::processByte(*this, _u8, c, 1, 1, 1);
      }
      void setPower(bool on) { ssd1306_command(on ? 0xAF : 0xAE); }
    private:
      utf8cp866::Decoder _u8;
    };
    RusSSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  #endif
#else
  StubDisplay display(SCREEN_WIDTH, SCREEN_HEIGHT);
#endif