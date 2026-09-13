#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include "ed25519/ed_25519.h"   // Nightcracker ed25519: X25519 key exchange для лички

float cpuTempC();

uint32_t crc32_upd(uint32_t crc, const uint8_t* data, size_t len);
uint16_t crc16buf(const uint8_t* data, size_t len);

int encryptGroupText(const uint8_t* secret32, uint8_t* dest, const uint8_t* src, int src_len);
String decryptGroupText(const uint8_t* secret32, uint8_t* mac, uint8_t* ciphertext, int len);

// Расшифровка raw OTA: возвращает сырые байты (без парсинга timestamp+type)
int decryptRaw(const uint8_t* secret32, const uint8_t* mac, const uint8_t* ciphertext, int len, uint8_t* out, int out_size);

int privateKeyTo16(const String& keyb64, uint8_t key16[16]);

void jsonEscape(const char* in, char* out, size_t outlen);

// Печать вещественных БЕЗ %f. Формат с плавающей точкой тянет в образ ~35 КБ newlib:
// float-вариант vfprintf, dtoa и общий вспомогательный код, который заодно затаскивает
// scanf, хотя мы его нигде не вызываем. Здесь печатаем целую и дробную части целыми.
char* fmtFix(float v, uint8_t dec, char* buf, size_t n);
// Разбор числа без strtod по той же причине
float parseFixed(const char* s);
