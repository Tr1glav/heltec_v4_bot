#pragma once
#include "config.h"
#if FEATURE_MESH_IP
#include <Arduino.h>
#include <stdint.h>

// Линк «IP over mesh» — надёжная доставка фрагментированных IP-датаграмм
// поверх сенсорного канала. Текстовый формат: "ip:d" / "ip:a".
// Одна активная пир-пара за раз (компаньон ↔ координатор).

// === Инициализация (вызвать из setup()) ===
void meshIpInit();

// === Точка входа принятого текста сенсорного канала ===
// Вызывается из parseMeshCorePacket. Вернёт true, если сообщение туннеля.
bool meshIpOnChannelText(const String& name, const String& text);

// === Очередь IP-датаграммы на отправку по линку ===
// Потокобезопасна (использует critical section); можно вызывать из ISR/task.
void meshIpInject(const uint8_t* pkt, uint16_t len);

// === Регистрация получателя собранных IP-датаграмм ===
// Вызывается один раз при инициализации (companion AP / coordinator NAT).
typedef void (*meshIpRecvCb)(const uint8_t* pkt, uint16_t len);
void meshIpSetRecvCb(meshIpRecvCb cb);

// === Сторожевой движок ===
// Ретрансмиссии, ACK, разбор очереди. Вызывать часто из главного цикла.
void meshIpTick();

// === Вспомогательные ===
uint16_t meshIpFragCap();    // макс. полезных байт в одном фрагменте
bool     meshIpLinkUp();     // есть ли видимый пир
void     meshIpReset();      // полный сброс линка

// === Роль-зависимый glue (компиляется условно) ===

// Компаньон: WiFi AP
#if defined(COMPANION_NODE)
void     meshIpApInit();
void     meshIpApStart();
void     meshIpApStop();
bool     meshIpApActive();
const char* meshIpApSsid();
const char* meshIpApPass();
#endif

// Координатор: NAT через raw pcb
#if defined(MQTT_ENABLED)
void     meshIpNatInit();
#endif

// === Чексум-хелперы (для NAT-модуля) ===
uint16_t meshIpChecksum(const uint8_t* data, uint16_t len);
uint16_t meshIpTcpUdpChecksum(const uint8_t* src_ip, const uint8_t* dst_ip,
                               uint8_t proto, const uint8_t* data, uint16_t len);

#endif // FEATURE_MESH_IP
