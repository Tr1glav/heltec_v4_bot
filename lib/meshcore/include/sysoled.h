#ifndef SYSOLED_H
#define SYSOLED_H

// Компактный драйвер SH1106 (132-колоночные панели) поверх Adafruit_GFX.
// На SSD1306-прошивке такие панели дают «горизонтальные полосы без текста»:
// не та геометрия колонок/страниц. Буфер 128x64, передача постранично.
// API совместим с Adafruit_SSD1306 для наших вызовов (begin/dim/clearDisplay/
// display/всё из Adafruit_GFX).

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "cyrillic.h"

// Само обозначение 128x64: видимая область начинается с DDRAM-колонки 0.
#define SH1106_COL_OFFSET 0

class SysOled : public Adafruit_GFX {
public:
  SysOled(int16_t w, int16_t h)
      : Adafruit_GFX(w, h) {
    memset(_buf, 0, sizeof(_buf));
  }

  // vcc игнорируется (включаем charge pump всегда), addr = I2C-адрес
  bool begin(uint8_t vcc, uint8_t addr) {
    (void)vcc;
    _addr = addr;
    _initialized = true;
    command(0xAE);                    // display off
    command(0xD5); command(0x80);     // clock div
    command(0xA8); command(0x3F);     // multiplex 1/64
    command(0xD3); command(0x00);     // display offset
    command(0x40);                    // start line 0
    command(0xA1);                    // segment remap
    command(0xC8);                    // COM scan dir
    command(0xDA); command(0x12);     // COM pins
    command(0x81); command(_contrast); // contrast
    command(0xD9); command(0xF1);     // precharge
    command(0xDB); command(0x40);     // VCOM deselect
    command(0xA4);                    // resume RAM content
    command(0xA6);                    // normal, not inverted
    command(0x8D); command(0x14);     // charge pump on
    command(0x20); command(0x02);     // page addressing mode
    command(0xAF);                    // display on
    return true;
  }

  void dim(bool d) {
    _contrast = d ? 0x00 : 0xCF;
    command(0x81);
    command(_contrast);
  }

  // Выключить/включить экран (RAM сохраняется, повторная инициализация не нужна)
  void setPower(bool on) {
    command(on ? 0xAF : 0xAE);   // display on / display off
  }

  void clearDisplay(void) {
    memset(_buf, 0, sizeof(_buf));
  }

  void display(void) {
    if (!_initialized) return;
    // Пишем ровно 128 колонок постранично. НЕ дописываем до 132: у SH1106
    // счётчик при выходе за 131 заворачивается на колонку 0, и пока контроллер
    // сканирует левый край, транзитная запись даёт мерцание первого символа.
    for (uint8_t page = 0; page < (uint8_t)(HEIGHT / 8); page++) {
      command(0xB0 | page);                     // page address
      command(0x00 | (SH1106_COL_OFFSET & 0x0F)); // column low nibble
      command(0x10 | ((SH1106_COL_OFFSET >> 4) & 0x0F)); // column high nibble
      const uint8_t* p = &_buf[page * WIDTH];
      dataRaw(p, WIDTH / 2);                    // 64 байта (лимит I2C-буфера)
      dataRaw(p + WIDTH / 2, WIDTH / 2);
    }
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    if ((x < 0) || (y < 0) || (x >= _width) || (y >= _height)) return;
    switch (getRotation()) {
      case 0: break;
      case 1: x = _width - 1 - x; break;
      case 2: x = _width - 1 - x; y = _height - 1 - y; break;
      case 3: y = _height - 1 - y; break;
    }
    uint16_t idx = x + (y >> 3) * _width;
    uint8_t bit = 1 << (y & 7);
    if (color) _buf[idx] |= bit;
    else       _buf[idx] &= ~bit;
  }

  // UTF-8 -> CP866 decode hook: ASCII -> classic font, Cyrillic -> 6x8 table.
  size_t write(uint8_t c) override {
    return utf8cp866::processByte(*this, _u8, c, 1, 1, 1);
  }

private:
  utf8cp866::Decoder _u8;
  static const uint16_t WIDTH = 128;
  static const uint16_t HEIGHT = 64;

  uint8_t _buf[WIDTH * HEIGHT / 8];
  uint8_t _addr = 0x3C;
  uint8_t _contrast = 0xCF;
  bool _initialized = false;

  void command(uint8_t c) {
    Wire.beginTransmission(_addr);
    Wire.write(0x00); // control byte: command stream
    Wire.write(c);
    Wire.endTransmission();
  }

  void dataRaw(const uint8_t* bytes, uint8_t n) {
    Wire.beginTransmission(_addr);
    Wire.write(0x40); // control byte: data stream
    Wire.write(bytes, n);
    Wire.endTransmission();
  }
};

#endif