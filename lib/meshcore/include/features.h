#pragma once

// ===== ПРИЗНАКИ СБОРКИ =====
// Роль платы («координатор», «сенсор», «компаньон») — это не одно свойство, а набор
// независимых возможностей. Раньше код ветвился прямо по ролям (MQTT_ENABLED,
// SENSOR_NODE, COMPANION_NODE), и любое сочетание вне трёх заготовленных собрать было
// нельзя: например, координатор без веб-страницы или сенсор без экрана.
//
// Здесь роли превращаются в отдельные признаки. Каждый можно задать в platformio.ini
// явно (-DFEATURE_WEB=0 и подобное), а если он не задан — он выводится из роли, как было
// раньше. Поэтому переход ничего не ломает: старые окружения собираются ровно так же.
//
// Проверять признаки нужно через #if, а не #ifdef: они всегда определены, но могут быть
// равны нулю.

// --- сеть и её потребители ---
#ifndef FEATURE_WIFI
  #ifdef MQTT_ENABLED
    #define FEATURE_WIFI 1
  #else
    #define FEATURE_WIFI 0
  #endif
#endif

#ifndef FEATURE_MQTT          // публикация в MQTT и автообнаружение в Home Assistant
  #define FEATURE_MQTT FEATURE_WIFI
#endif

#ifndef FEATURE_WEB           // страница OTA на порту 3232
  #define FEATURE_WEB FEATURE_WIFI
#endif

#ifndef FEATURE_AUTOUPDATE    // проверка релизов GitHub и раздача прошивок узлам
  #define FEATURE_AUTOUPDATE FEATURE_WIFI
#endif

#ifndef FEATURE_NTP           // синхронизация часов и рассылка времени узлам
  #define FEATURE_NTP FEATURE_WIFI
#endif

// --- прошивка по радио ---
#ifndef FEATURE_MESH_OTA_SENDER    // сторона, раздающая образ (координатор)
  #define FEATURE_MESH_OTA_SENDER FEATURE_WIFI
#endif

#ifndef FEATURE_MESH_OTA_RECEIVER  // сторона, принимающая образ (узел)
  #ifdef SENSOR_NODE
    #define FEATURE_MESH_OTA_RECEIVER 1
  #else
    #define FEATURE_MESH_OTA_RECEIVER 0
  #endif
#endif

// --- поведение узла ---
#ifndef FEATURE_SENSOR        // heartbeat, кнопка, проверка связи, настройка по радио
  #ifdef SENSOR_NODE
    #define FEATURE_SENSOR 1
  #else
    #define FEATURE_SENSOR 0
  #endif
#endif

#ifndef FEATURE_COMPANION     // BLE и протокол телефонного приложения
  #ifdef COMPANION_NODE
    #define FEATURE_COMPANION 1
  #else
    #define FEATURE_COMPANION 0
  #endif
#endif

#ifndef FEATURE_POWERSAVE     // гашение экрана и пониженная частота процессора
  #define FEATURE_POWERSAVE FEATURE_SENSOR
#endif

// --- железо ---
#ifndef FEATURE_DISPLAY
  #define FEATURE_DISPLAY HAS_OLED
#endif

#ifndef FEATURE_BUTTON
  #define FEATURE_BUTTON FEATURE_SENSOR
#endif

// Проверки сочетаний: молча собрать бессмысленную прошивку хуже, чем не собрать вовсе.
#if FEATURE_MQTT && !FEATURE_WIFI
  #error "FEATURE_MQTT требует FEATURE_WIFI"
#endif
#if FEATURE_WEB && !FEATURE_WIFI
  #error "FEATURE_WEB требует FEATURE_WIFI"
#endif
#if FEATURE_AUTOUPDATE && !FEATURE_WIFI
  #error "FEATURE_AUTOUPDATE требует FEATURE_WIFI"
#endif
#if FEATURE_COMPANION && !FEATURE_SENSOR
  #error "Компаньон собирается поверх сенсорного узла: нужен FEATURE_SENSOR"
#endif
