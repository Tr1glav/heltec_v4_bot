#pragma once

#include "config.h"

#if FEATURE_SENSOR
// Периодические задачи узла: зовётся из главного цикла, не блокирует
void sensorTasksTick();
#endif
