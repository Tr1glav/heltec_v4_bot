#pragma once

#include "config.h"

#if FEATURE_MQTT || FEATURE_AUTOUPDATE || FEATURE_NTP
// Периодические задачи координатора: зовётся из главного цикла, не блокирует
void coordinatorTasksTick();
#endif
