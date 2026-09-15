#pragma once

#include "config.h"

bool initLoRa();
void radioSetParams(float freq, float bw, int sf, int cr);
void radioSetNormalConfig();
void radioSetFastConfig();
// true, если последний переход в быстрый режим действительно удался (beginFSK без ошибок)
bool radioFastReadyNow();
void rearmRadioAGC();
int txFrame(uint8_t* frame, int f);

// Опрос радио и разбор принятого — зовётся из главного цикла
void radioRxTick();
