#pragma once

#include "config.h"

bool initLoRa();
void radioSetParams(float freq, float bw, int sf, int cr);
void radioSetNormalConfig();
void radioSetFastConfig();
void rearmRadioAGC();
int txFrame(uint8_t* frame, int f);
