#pragma once

#include <Arduino.h>

#ifdef COMPANION_NODE
// Компаньон: узел, к которому подключается телефонное приложение MeshCore по BLE.
// Протокол совместим с оригиналом (docs/companion_protocol.md): сервис Nordic UART,
// кадр = [код][данные], числа little-endian, до 176 байт.
//
// Сенсорные функции (hello, пинг по тройному нажатию, прошивка по LoRa) остаются:
// окружение компаньона определяет и SENSOR_NODE, поэтому тот код собирается как есть.
void companionBegin();
void companionTick();
// Код сопряжения BLE — случайный при каждом запуске, показывается на экране
uint32_t companionBlePin();
// true — приложение подключено; пока нет, экран занят кодом сопряжения
bool companionBleLinked();
// Входящее сообщение из сенсорного/группового канала — кладём в очередь для приложения
// notify = false для того, что отправило само устройство: сообщение попадёт в переписку,
// но телефон не покажет уведомление о собственном же действии.
void companionOnChannelText(int channelIdx, const String& text, float snr, uint8_t pathLen,
                            bool notify = true);
// Услышан адверт: обновляем список контактов и сообщаем об этом приложению.
// app — содержимое поля приложения адверта: [флаги|тип][имя], applen — его длина.
void companionOnAdvert(const uint8_t* pub, const uint8_t* app, int applen,
                       uint8_t pathLen, const uint8_t* path);
#endif
