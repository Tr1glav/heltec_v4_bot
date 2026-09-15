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
#include "globals.h"
#include "display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>   // esp_netif_get_netif_impl
#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <lwip/raw.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>

static esp_netif_t*         s_apNetif    = NULL;
static esp_netif_ip_info_t  s_apIp;
static struct netif*        s_apLwip    = NULL;
static bool                 s_apActive  = false;
static String               s_ssid;
static String               s_pass;

static esp_err_t apRxGrab(void* buffer, uint16_t len, void* eb);

// Колбэк собранной из туннеля датаграммы — определён ниже, нужен meshIpApStart.
static void meshIpApRecvCb(const uint8_t* pkt, uint16_t len);

// ===================== Жизненный цикл AP =====================

void meshIpApInit() {
    // AP включается пользователем (двойное нажатие), при старте ничего не делаем.
}

const char* meshIpApSsid() { return s_ssid.c_str(); }
const char* meshIpApPass() { return s_pass.c_str(); }
bool meshIpApActive()      { return s_apActive; }

void meshIpApStart() {
    if (s_apActive) return;
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    char ssidBuf[32];
    snprintf(ssidBuf, sizeof(ssidBuf), "mesh-%02X%02X", mac[4], mac[5]);
    uint32_t seed = (millis() ^ ((uint32_t)mac[2] << 16 | (uint32_t)mac[3])) & 0x7FFFFFFF;
    char passBuf[16];
    for (int i = 0; i < 8; i++) {
        seed = seed * 1103515245 + 12345;
        passBuf[i] = (char)('0' + (seed / 65536) % 10);
    }
    passBuf[8] = 0;
    s_ssid = String(ssidBuf);
    s_pass = String(passBuf);

    if (!WiFi.softAP(ssidBuf, passBuf)) {
        Serial.println("[IP] softAP FAILED");
        return;
    }
    delay(100);  // дать AP-интерфейсу подняться (DHCP-сервер, netif)

    s_apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_apNetif) {
        esp_netif_get_ip_info(s_apNetif, &s_apIp);
        s_apLwip = (struct netif*)esp_netif_get_netif_impl(s_apNetif);
    }
    if (!s_apLwip) {
        Serial.println("[IP] AP netif not found");
        WiFi.softAPdisconnect(true);
        return;
    }

    // Перехват RX-буфера AP. Кадры в «чужую» сеть забираем в туннель, остальное
    // (ARP/DHCP/к себе) прогоняем через esp_netif_receive — ровно как делал бы
    // штатный wifi_ap_receive.
    esp_wifi_internal_reg_rxcb(WIFI_IF_AP, apRxGrab);

    // Собранные из туннеля IP-датаграммы уходят в телефон
    meshIpSetRecvCb(meshIpApRecvCb);

    s_apActive = true;
    Serial.printf("[IP] AP ssid=%s pass=%s gw=%ld.%ld.%ld.%ld\n",
                  ssidBuf, passBuf,
                  (long)(s_apIp.ip.addr >> 24) & 0xFF, (long)(s_apIp.ip.addr >> 16) & 0xFF,
                  (long)(s_apIp.ip.addr >> 8) & 0xFF, (long)s_apIp.ip.addr & 0xFF);
    screenWake();
}

void meshIpApStop() {
    if (!s_apActive) return;
    s_apActive = false;
    // При остановке AP слот rxcb освобождается драйвером сам; переустанавливаем
    // перехват при следующем старте (idempotent). Старый callback не трогаем.
    WiFi.softAPdisconnect(true);
}

// ===================== Захват входящих кадров телефона =====================

static esp_err_t apRxGrab(void* buffer, uint16_t len, void* eb) {
    if (!s_apNetif || !buffer || len < 14) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    uint8_t* b = (uint8_t*)buffer;

    // Ищем ethertype: обычный 802.3 (type на off 12) или LLC/SNAP (type на off 20)
    int typeOff = -1;
    if ((b[12] == 0x08 && b[13] == 0x00) || (b[12] == 0x08 && b[13] == 0x06) ||
        (b[12] == 0x86 && b[13] == 0xDD)) {
        typeOff = 12;
    } else if (len >= 22 && b[12] == 0xAA && b[13] == 0xAA && b[14] == 0x03 &&
               b[15] == 0x00 && b[16] == 0x00 && b[17] == 0x00) {
        typeOff = 20;   // SNAP: LLC aa aa 03 + OUI 00 00 00 + ethertype
    } else {
        esp_netif_receive(s_apNetif, buffer, len, eb);
        return ESP_OK;
    }

    uint16_t etype = (uint16_t)((b[typeOff] << 8) | b[typeOff + 1]);
    if (etype != 0x0800) {               // ARP, IPv6 и пр. — lwIP сам
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

    // Локальная цель (шлюзу/DHCP/broadcast) — пусть обработает lwIP
    if (dst == 0 || dst == 0xFFFFFFFF || dst == s_apIp.ip.addr) {
        esp_netif_receive(s_apNetif, buffer, len, eb);
        return ESP_OK;
    }

    // Датаграмма в «внешний» мир — в туннель
    int takeLen = (int)iplen;
    if (takeLen > MESH_IP_PKT_MAX) takeLen = MESH_IP_PKT_MAX;
    uint8_t pkt[MESH_IP_PKT_MAX];
    memcpy(pkt, ip, (size_t)takeLen);
    esp_wifi_internal_free_rx_buffer(eb);
    meshIpInject(pkt, (uint16_t)takeLen);
    return ESP_OK;
}

// ===================== Даунлинк к телефону =====================

static struct raw_pcb* s_rawSend[18] = {0};

static struct raw_pcb* apGetOrCreateRaw(uint8_t proto) {
    if (proto > 17) return NULL;
    if (!s_rawSend[proto]) {
        struct raw_pcb* pcb = raw_new_ip_type(IPADDR_TYPE_V4, proto);
        if (!pcb) return NULL;
        raw_bind_netif(pcb, s_apLwip);   // отправка — строго через AP-интерфейс
        s_rawSend[proto] = pcb;
    }
    return s_rawSend[proto];
}

// Полный IPv4-датаграммы для телефона (src=шлюз, dst=телефон). Отправляем
// транспортный сегмент через raw_sendto_if_src — lwIP соберёт IP-заголовок.
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
    if (!pcb) return;
    struct pbuf* p = pbuf_alloc(PBUF_RAW, segLen, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, ip + ihl, segLen);

    ip_addr_t src_ip, dst_ip;
    IP_ADDR4(&src_ip, ip[12], ip[13], ip[14], ip[15]);
    IP_ADDR4(&dst_ip, ip[16], ip[17], ip[18], ip[19]);
    err_t err = raw_sendto_if_src(pcb, p, &dst_ip, s_apLwip, &src_ip);
    if (err != ERR_OK) {
        Serial.printf("[IP] ap send err=%d\n", (int)err);
    }
    pbuf_free(p);
}

static void meshIpApRecvCb(const uint8_t* pkt, uint16_t len) {
    if (!s_apActive || len > sizeof(s_sendPkt)) return;
    memcpy(s_sendPkt, pkt, len);
    s_sendLen = len;
    tcpip_callback(apSendToPhoneTcpip, NULL);
}

#endif // FEATURE_MESH_IP && COMPANION_NODE