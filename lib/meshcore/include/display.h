#pragma once
#include "config.h"
void drawIdleStatus();
float batteryVoltage();   // вольты; 0 — платы без измерения батареи
int batteryPercent();      // 0..100; -1 — измерения нет или аккумулятор не подключён
bool batteryPresent();     // false — напряжение около нуля, батареи нет
