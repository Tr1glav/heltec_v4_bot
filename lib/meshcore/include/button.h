#pragma once

#include "config.h"

#if FEATURE_BUTTON
// Опрос кнопки: зовётся из главного цикла, ничего не блокирует
void buttonTick();
#endif
