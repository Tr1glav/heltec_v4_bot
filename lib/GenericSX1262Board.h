#pragma once

// Minimal MeshCore board adapter for "generic ESP32-S3 + SX1262" builds
// (see platformio.ini env generic_sx1262). No OLED/FEM/VEXT, serial only.
// Only used because MeshCore's ESP32Board.cpp needs a concrete MainBoard
// subclass to compile; the instance itself is never used at runtime.

#include <Arduino.h>
#include <helpers/ESP32Board.h>

class GenericSX1262Board : public ESP32Board {

public:
  uint16_t getBattMilliVolts() override { return 0; }
  const char* getManufacturerName() const override { return "GenericSX1262"; }
};