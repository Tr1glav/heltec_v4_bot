#include "config.h"
#include "crypto.h"
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

float cpuTempC() {
    return temperatureRead() - (float)TEMP_SENSOR_OFFSET;
}

uint32_t crc32_upd(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return crc;
}

uint16_t crc16buf(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

int encryptGroupText(const uint8_t* secret32, uint8_t* dest, const uint8_t* src, int src_len) {
    if (src_len <= 0) return 0;
    int padded = (src_len + 15) & ~15;
    uint8_t* cipher = dest + 2;  // MAC спереди

    // AES-128-ECB по блокам, хвост добиваем нулями
    uint8_t block[16];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, secret32, 128);
    for (int i = 0; i < padded; i += 16) {
        memset(block, 0, 16);
        memcpy(block, src + i, min(16, src_len - i));
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, cipher + i);
    }
    mbedtls_aes_free(&aes);

    // HMAC-SHA256(cipher) с полным 32-байтным секретом
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32, cipher, padded, hmac_out);
    dest[0] = hmac_out[0];
    dest[1] = hmac_out[1];
    return 2 + padded;
}

String decryptGroupText(const uint8_t* secret32, uint8_t* mac, uint8_t* ciphertext, int len) {
    if (len <= 0 || len % 16 != 0) return "";

    // Проверяем HMAC-SHA256(ciphertext) с полным 32-байтным секретом
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32,
                    ciphertext, len, hmac_out);
    if (hmac_out[0] != mac[0] || hmac_out[1] != mac[1]) return "";

    // AES-128-ECB расшифровка
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, secret32, 128);

    uint8_t plaintext[256];
    for (int i = 0; i < len; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ciphertext + i, plaintext + i);
    }
    mbedtls_aes_free(&aes);

    // plaintext: [timestamp 4B][txt_type 1B][text...]
    String message = "";
    for (int i = 5; i < len; i++) {
        if (plaintext[i] == 0) break;
        message += (char)plaintext[i];
    }
    return message;
}

int decryptRaw(const uint8_t* secret32, const uint8_t* mac, const uint8_t* ciphertext, int len, uint8_t* out, int out_size) {
    if (len <= 0 || len % 16 != 0) return 0;

    // Проверяем HMAC-SHA256(ciphertext)
    uint8_t hmac_out[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    secret32, 32, ciphertext, len, hmac_out);
    if (hmac_out[0] != mac[0] || hmac_out[1] != mac[1]) return 0;

    // AES-128-ECB расшифровка
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, secret32, 128);
    int n = min(len, out_size);
    for (int i = 0; i < n; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ciphertext + i, out + i);
    }
    mbedtls_aes_free(&aes);
    return n;
}

int privateKeyTo16(const String& keyb64, uint8_t key16[16]) {
    if (keyb64.length() == 0) return -1;   // автоключ
    size_t olen = 0;
    uint8_t tmp[32];
    int rc = mbedtls_base64_decode(tmp, sizeof(tmp), &olen,
                                   (const uint8_t*)keyb64.c_str(), keyb64.length());
    if (rc != 0 || olen != 16) {
        Serial.printf("[PRV] bad PSK '%s' (need 16 raw bytes in base64), using auto key\n", keyb64.c_str());
        return -1;
    }
    memcpy(key16, tmp, 16);
    return 1;                              // PSK-ключ
}

char* fmtFix(float v, uint8_t dec, char* buf, size_t n) {
    if (n == 0) return buf;
    long scale = 1;
    for (uint8_t i = 0; i < dec; i++) scale *= 10;
    long x = (long)(v * scale + (v >= 0 ? 0.5f : -0.5f));
    const char* sign = "";
    if (x < 0) { x = -x; sign = "-"; }
    if (dec == 0) snprintf(buf, n, "%s%ld", sign, x);
    else          snprintf(buf, n, "%s%ld.%0*ld", sign, x / scale, (int)dec, x % scale);
    return buf;
}

float parseFixed(const char* s) {
    while (*s == ' ') s++;
    bool neg = (*s == '-');
    if (neg || *s == '+') s++;
    long ip = 0;
    while (*s >= '0' && *s <= '9') ip = ip * 10 + (*s++ - '0');
    float frac = 0.0f, scale = 0.1f;
    if (*s == '.' || *s == ',') {
        s++;
        while (*s >= '0' && *s <= '9') { frac += (*s++ - '0') * scale; scale *= 0.1f; }
    }
    float v = (float)ip + frac;
    return neg ? -v : v;
}

void jsonEscape(const char* in, char* out, size_t outlen) {
    size_t n = 0;
    for (size_t i = 0; in[i] != 0 && n + 3 < outlen; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
        else if ((uint8_t)c < 0x20) { /* остальные управляющие пропускаем */ }
        else out[n++] = c;
    }
    out[n] = 0;
}
