#pragma once
#include "config.h"
void drawIdleStatus();
#ifdef SENSOR_NODE
// Экран узла гаснет в простое и просыпается от длинного нажатия кнопки
void screenWake();
bool screenIsOn();
void screenTick();
#endif
float batteryVoltage();   // вольты; 0 — платы без измерения батареи
int batteryPercent();      // 0..100; -1 — измерения нет или аккумулятор не подключён
bool batteryPresent();     // false — напряжение около нуля, батареи нет
