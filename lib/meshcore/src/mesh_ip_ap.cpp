// ===== IP over MeshCore — компаньон: WiFi AP + захват аплинка =====
// Компаньон по двойному нажатию кнопки поднимает softAP и перехватывает исходящие
// IP-кадры телефона через esp_wifi_internal_reg_rxcb(WIFI_IF_AP). Кадры в «внешний»
// мир уходят в mesh-туннель (meshIpInject), а ARP/DHCP/локальные — передаются lwIP
// через esp_netif_receive как обычно. Даунлинк (пакеты из туннеля) уходит в телефон
// через raw_sendto_if_src на AP-netif: lwIP сам соберёт IP-заголовок и найдёт MAC
// телефона в ARP-таблице.

#include "config.h"
#if FEATURE_MESH_IP && defined(COMPANION_NODE)

#include "mesh_ip.h"
#include "companion.h"   // companionBleStop / companionBegin
#include "globals.h"
#include "display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <lwip/raw.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>

// ===================== Состояние =====================

static esp_netif_t*         s_apNetif    = NULL;
static esp_netif_ip_info_t  s_apIp;
static uint32_t             s_apGw   = 0;   // GW в порядке байт пакета (big-endian uint32)
static struct netif*        s_apLwip    = NULL;
static bool                 s_apActive  = false;
static String               s_ssid;
static String               s_pass;
static bool                 s_inited    = false;   // пароль сгенерирован при старте

static esp_err_t apRxGrab(void* buffer, uint16_t len, void* eb);
static void meshIpApRecvCb(const uint8_t* pkt, uint16_t len);

// ===================== Жизненный цикл AP =====================

void meshIpApInit() {
    // Пароль генерируется ОДИН раз при включении ноды — чтобы можно было
    // переподключиться после потери связи без повторного нажатия кнопки.
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);   // MAC STA стабильнее, чем AP

    char ssidBuf[32];
    snprintf(ssidBuf, sizeof(ssidBuf), "mesh-%02X%02X", mac[4], mac[5]);

    // Детерминированный пароль: MAC + startup millis
    uint32_t seed = (uint32_t)millis();
    for (int i = 0; i < 6; i++) seed ^= (uint32_t)mac[i] << ((i & 3) * 8);
    char passBuf[16];
    for (int i = 0; i < 8; i++) {
        seed = seed * 1103515245 + 12345;
        passBuf[i] = (char)('0' + (seed / 65536) % 10);
    }
    passBuf[8] = 0;

    s_ssid = String(ssidBuf);
    s_pass = String(passBuf);
    s_inited = true;
    Serial.printf("[IP] pass=%s generated at boot\n", passBuf);
}

const char* meshIpApSsid() { return s_ssid.c_str(); }
const char* meshIpApPass() { return s_pass.c_str(); }
bool meshIpApActive()      { return s_apActive; }

void meshIpApStart() {
    if (s_apActive) return;
    if (!s_inited) { Serial.println("[IP] not inited!"); return; }

    Serial.println("[IP] === AP START ===");

    // 1) Полностью выключаем BLE — освобождаем ~70 КБ RAM + освобождаем радио
    Serial.println("[IP] stopping BLE...");
    companionBleStop();

    // 2) Максимальная частота процессора — вся мощность на WiFi + LoRa
    setCpuFrequencyMhz(CPU_MHZ_FAST);
    Serial.printf("[IP] CPU → %d MHz\n", CPU_MHZ_FAST);

    // 3) Поднимаем WiFi AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(s_ssid.c_str(), s_pass.c_str());
    delay(200);   // дать AP подняться (DHCP-сервер, netif)

    s_apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_apNetif) {
        esp_netif_get_ip_info(s_apNetif, &s_apIp);
        s_apGw = ntohl(s_apIp.ip.addr);   // ← lwIP хранит в network order; ntohl() → big-endian uint32, как构造 dst из пакета
        s_apLwip = (struct netif*)esp_netif_get_netif_impl(s_apNetif);
    }
    if (!s_apLwip) {
        Serial.println("[IP] AP netif FAIL — rollback");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        setCpuFrequencyMhz(CPU_MHZ_IDLE);
        companionBegin();
        return;
    }

    // 4) Перехват RX-буфера AP: гребём «чужой» IPv4 в туннель,
    //    ARP/DHCP/локальное — в lwIP как обычно.
    esp_err_t rc = esp_wifi_internal_reg_rxcb(WIFI_IF_AP, apRxGrab);
    Serial.printf("[IP] reg_rxcb(WIFI_IF_AP) rc=0x%X  (%s)\n", rc,
                  rc == ESP_OK ? "OK" : "FAIL");
    if (rc != ESP_OK) {
        Serial.println("[IP] WARNING: rxcb NOT installed — телефон не сможет ходить в туннель");
    }

    // 5) Даунлинк: из туннеля → телефон
    meshIpSetRecvCb(meshIpApRecvCb);

    s_apActive = true;
    Serial.printf("[IP] AP UP  ssid=%s pass=%s gw=%d.%d.%d.%d\n",
                  s_ssid.c_str(), s_pass.c_str(),
                  s_apIp.ip.addr & 0xFF, (s_apIp.ip.addr >> 8) & 0xFF,
                  (s_apIp.ip.addr >> 16) & 0xFF, (s_apIp.ip.addr >> 24) & 0xFF);
    screenWake();
}

void meshIpApStop() {
    if (!s_apActive) return;
    Serial.println("[IP] === AP STOP ===");
    s_apActive = false;

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    // Восстанавливаем BLE
    companionBegin();
    Serial.println("[IP] BLE restarted");

    // Восстанавливаем частоту
    setCpuFrequencyMhz(CPU_MHZ_IDLE);
    Serial.printf("[IP] CPU → %d MHz\n", CPU_MHZ_IDLE);
    screenWake();
}

// ===================== Захват входящих кадров телефона =====================

uint32_t g_meshIpRxFwd = 0;   // forwarded to lwIP (ARP/DHCP/local)
uint32_t g_meshIpRxTun = 0;   // injected into tunnel
static uint32_t s_rxTotal = 0; // all calls into apRxGrab

static esp_err_t apRxGrab(void* buffer, uint16_t len, void* eb) {
    if (!s_apNetif || !buffer || len < 14) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    uint8_t* b = (uint8_t*)buffer;
    s_rxTotal++;

    // Первые 20 пакетов — логируем каждый, чтобы понять что вообще приходит
    if (s_rxTotal <= 20) {
        Serial.printf("[IP] apRxGrab #%lu len=%d eth=%02X%02X\n",
                      s_rxTotal, len, b[12], b[13]);
    }

    // Ищем ethertype: обычный 802.3 (type на off 12) или LLC/SNAP (type на off 20)
    int typeOff = -1;
    if ((b[12] == 0x08 && b[13] == 0x00) || (b[12] == 0x08 && b[13] == 0x06) ||
        (b[12] == 0x86 && b[13] == 0xDD)) {
        typeOff = 12;
    } else if (len >= 22 && b[12] == 0xAA && b[13] == 0xAA && b[14] == 0x03 &&
               b[15] == 0x00 && b[16] == 0x00 && b[17] == 0x00) {
        typeOff = 20;
    } else {
        g_meshIpRxFwd++;
        if (s_rxTotal <= 20)
            Serial.printf("[IP] fwd #%lu: unknown-eth, %dB\n", s_rxTotal, len);
        esp_netif_receive(s_apNetif, buffer, len, eb);
        return ESP_OK;
    }

    uint16_t etype = (uint16_t)((b[typeOff] << 8) | b[typeOff + 1]);
    if (etype != 0x0800) {
        g_meshIpRxFwd++;
        if (s_rxTotal <= 20)
            Serial.printf("[IP] fwd #%lu: non-IPv4 eth=0x%04X, %dB\n", s_rxTotal, etype, len);
        esp_netif_receive(s_apNetif, buffer, len, eb);
        return ESP_OK;
    }

    const uint8_t* ip = b + typeOff + 2;
    uint8_t  ihl  = (uint8_t)((ip[0] & 0x0F) * 4);
    uint16_t iplen = len - (uint16_t)(typeOff + 2);
    if (ihl < 20 || iplen < ihl) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    uint32_t dst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
                   ((uint32_t)ip[18] << 8) | (uint32_t)ip[19];

    // Локальная цель (шлюзу/DHCP/broadcast) — lwIP обработает
    if (dst == 0 || dst == 0xFFFFFFFF || dst == s_apGw) {
        g_meshIpRxFwd++;
        if (s_rxTotal <= 20 || (g_meshIpRxFwd & 0x0F) == 1)
            Serial.printf("[IP] fwd #%lu: dst=%d.%d.%d.%d %s, %dB\n",
                          s_rxTotal, ip[16], ip[17], ip[18], ip[19],
                          etype == 0x0800 ? "IPv4-ICMP/UDP" : "?", len);
        esp_netif_receive(s_apNetif, buffer, len, eb);
        return ESP_OK;
    }

    // Внешний мир → туннель
    int takeLen = (int)iplen;
    if (takeLen > MESH_IP_PKT_MAX) takeLen = MESH_IP_PKT_MAX;
    uint8_t pkt[MESH_IP_PKT_MAX];
    memcpy(pkt, ip, (size_t)takeLen);
    esp_wifi_internal_free_rx_buffer(eb);
    meshIpInject(pkt, (uint16_t)takeLen);

    g_meshIpRxTun++;
    if ((g_meshIpRxTun & 0x1F) == 1) {   // раз в 32 пакета, чтобы не спамить
        uint32_t src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
                       ((uint32_t)ip[14] << 8) | (uint32_t)ip[15];
        Serial.printf("[IP] rx tun #%lu: %d.%d.%d.%d → %d.%d.%d.%d (%dB)\n",
                      g_meshIpRxTun,
                      (int)ip[12], (int)ip[13], (int)ip[14], (int)ip[15],
                      (int)ip[16], (int)ip[17], (int)ip[18], (int)ip[19],
                      takeLen);
        (void)src;
    }
    return ESP_OK;
}

// ===================== Даунлинк к телефону =====================

static struct raw_pcb* s_rawSend[18] = {0};

static struct raw_pcb* apGetOrCreateRaw(uint8_t proto) {
    if (proto > 17) return NULL;
    if (!s_rawSend[proto]) {
        struct raw_pcb* pcb = raw_new_ip_type(IPADDR_TYPE_V4, proto);
        if (!pcb) return NULL;
        raw_bind_netif(pcb, s_apLwip);
        s_rawSend[proto] = pcb;
    }
    return s_rawSend[proto];
}

static uint8_t  s_sendPkt[MESH_IP_PKT_MAX];
static uint16_t s_sendLen = 0;

static void apSendToPhoneTcpip(void* arg) {
    (void)arg;
    if (s_sendLen < 20) return;
    const uint8_t* ip = s_sendPkt;
    uint8_t proto = ip[9];
    if (proto != 1 && proto != 6 && proto != 17) return;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
    if (ihl < 20 || ihl >= s_sendLen) return;
    uint16_t segLen = s_sendLen - ihl;

    struct raw_pcb* pcb = apGetOrCreateRaw(proto);
    if (!pcb) { Serial.println("[IP] raw_new FAIL"); return; }
    struct pbuf* p = pbuf_alloc(PBUF_RAW, segLen, PBUF_RAM);
    if (!p) { Serial.println("[IP] pbuf_alloc FAIL"); return; }
    memcpy(p->payload, ip + ihl, segLen);

    ip_addr_t src_ip, dst_ip;
    IP_ADDR4(&src_ip, ip[12], ip[13], ip[14], ip[15]);
    IP_ADDR4(&dst_ip, ip[16], ip[17], ip[18], ip[19]);
    err_t err = raw_sendto_if_src(pcb, p, &dst_ip, s_apLwip, &src_ip);
    if (err != ERR_OK) {
        Serial.printf("[IP] → phone ERR %d  proto=%d seg=%d\n", (int)err, proto, segLen);
    } else {
        Serial.printf("[IP] → phone OK   proto=%d seg=%d %d.%d.%d.%d\n",
                      proto, segLen, ip[16], ip[17], ip[18], ip[19]);
    }
    pbuf_free(p);
}

static void meshIpApRecvCb(const uint8_t* pkt, uint16_t len) {
    if (!s_apActive || len > sizeof(s_sendPkt)) return;
    Serial.printf("[IP] recvCb %dB → phone\n", len);
    memcpy(s_sendPkt, pkt, len);
    s_sendLen = len;
    tcpip_callback(apSendToPhoneTcpip, NULL);
}

#endif // FEATURE_MESH_IP && COMPANION_NODE