// ===== IP over MeshCore — координатор: NAT + шлюз в интернет =====
// Координатор получает из туннеля IP-датаграмму телефона, подменяет src на свой
// STA-адрес (и порт на внешний), пересчитывает чексумы и уходит в интернет своим
// STA WiFi через raw_sendto_if_src. Ответы интернета ловит raw_recv (proto TCP/UDP/
// ICMP): по ext-порту находит исходного телефона, восстанавливает его адрес и гонит
// датаграмму в туннель. Обычный трафик координатора (MQTT, web-сервер) не трогаем —
// не наш ext-порт → возвращаем 0, lwIP обработает сам.

#include "config.h"
#if FEATURE_MESH_IP && defined(MQTT_ENABLED)

#include "mesh_ip.h"
#include "globals.h"
#include "radio.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>   // esp_netif_get_netif_impl
#include <lwip/raw.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>

// ===================== NAT-таблица =====================

#define MESH_NAT_MAX 8

struct NatEntry {
    bool     used;
    uint8_t  proto;        // 1/6/17
    uint32_t phone_ip;     // сетевой порядок
    uint16_t phone_port;   // host order
    uint16_t ext_port;     // host order, наш внешний
    uint32_t last_ms;
};
static NatEntry s_nat[MESH_NAT_MAX];
static uint16_t s_natPortCursor = 10000;

static struct raw_pcb* s_rawRecv[18] = {0};   // приём (raw_recv)
static struct raw_pcb* s_rawSend[18] = {0};   // отправка

static esp_netif_t*  s_staEspNetif = NULL;
static struct netif* s_staNetif    = NULL;

// Определены ниже, нужны meshIpNatInit.
static u8_t coordRawRecv(void* arg, struct raw_pcb* pcb, struct pbuf* p,
                         const ip_addr_t* src_ip);
static void natUpstreamCb(const uint8_t* pkt, uint16_t len);

// ===================== Таблица =====================

static NatEntry* natFindByExt(uint8_t proto, uint16_t extPort) {
    for (int i = 0; i < MESH_NAT_MAX; i++)
        if (s_nat[i].used && s_nat[i].proto == proto && s_nat[i].ext_port == extPort)
            return &s_nat[i];
    return NULL;
}

static NatEntry* natFindOrCreate(uint8_t proto, uint32_t phoneIp, uint16_t phonePort) {
    for (int i = 0; i < MESH_NAT_MAX; i++)
        if (s_nat[i].used && s_nat[i].proto == proto &&
            s_nat[i].phone_ip == phoneIp && s_nat[i].phone_port == phonePort)
            return &s_nat[i];
    // свободный слот или самый старый
    NatEntry* reuse = NULL;
    uint32_t oldest = 0;
    for (int i = 0; i < MESH_NAT_MAX; i++) {
        if (!s_nat[i].used) { reuse = &s_nat[i]; break; }
        if (!reuse || s_nat[i].last_ms < oldest) { reuse = &s_nat[i]; oldest = s_nat[i].last_ms; }
    }
    if (!reuse) return NULL;
    reuse->used       = true;
    reuse->proto      = proto;
    reuse->phone_ip   = phoneIp;
    reuse->phone_port = phonePort;
    reuse->ext_port   = s_natPortCursor++;
    if (s_natPortCursor > 16000) s_natPortCursor = 10000;   // кольцевые ext-порты
    reuse->last_ms = millis();
    Serial.printf("[NAT] map %d.%d.%d.%d:%u proto=%u -> ext %u\n",
                  (int)(phoneIp >> 24) & 0xFF, (int)(phoneIp >> 16) & 0xFF,
                  (int)(phoneIp >> 8) & 0xFF, (int)phoneIp & 0xFF,
                  phonePort, proto, reuse->ext_port);
    return reuse;
}

// ===================== Инициализация =====================

void meshIpNatInit() {
    memset(s_nat, 0, sizeof(s_nat));
    s_staEspNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_staEspNetif) {
        s_staNetif = (struct netif*)esp_netif_get_netif_impl(s_staEspNetif);
    }
    // raw_recv для TCP/UDP/ICMP
    for (uint8_t proto = 1; proto <= 17; proto += (proto == 1) ? 5 : 11) {
        if (proto != 1 && proto != 6 && proto != 17) continue;
        struct raw_pcb* pcb = raw_new_ip_type(IPADDR_TYPE_V4, proto);
        if (!pcb) { Serial.printf("[NAT] raw_new %u failed\n", proto); continue; }
        raw_recv(pcb, coordRawRecv, NULL);
        s_rawRecv[proto] = pcb;
    }
    Serial.printf("[NAT] init, sta_netif=%p\n", (void*)s_staNetif);
    meshIpSetRecvCb(natUpstreamCb);
}

static struct raw_pcb* natGetOrCreateSend(uint8_t proto) {
    if (proto > 17) return NULL;
    if (!s_rawSend[proto]) {
        struct raw_pcb* pcb = raw_new_ip_type(IPADDR_TYPE_V4, proto);
        if (!pcb) return NULL;
        s_rawSend[proto] = pcb;
    }
    return s_rawSend[proto];
}

// ===================== Upstream: телефон → интернет =====================

// Отправка сегмента в интернет. Вызывается в контексте tcpip_thread.
static uint8_t  s_upSeg[MESH_IP_PKT_MAX];
static uint16_t s_upSegLen  = 0;
static ip_addr_t s_upDst;
static ip_addr_t s_upSrc;
static uint8_t  s_upProto  = 0;

static void natSendUpTcpip(void* arg) {
    (void)arg;
    if (s_upSegLen == 0 || !s_staNetif) return;
    struct raw_pcb* pcb = natGetOrCreateSend(s_upProto);
    if (!pcb) return;
    struct pbuf* p = pbuf_alloc(PBUF_RAW, s_upSegLen, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, s_upSeg, s_upSegLen);
    err_t err = raw_sendto_if_src(pcb, p, &s_upDst, s_staNetif, &s_upSrc);
    if (err != ERR_OK) Serial.printf("[NAT] up send err=%d\n", (int)err);
    pbuf_free(p);
    s_upSegLen = 0;
}

// meshIpRecvCb: датаграмма телефона из туннеля (main-loop контекст)
static void natUpstreamCb(const uint8_t* pkt, uint16_t len) {
    if (len < 20 || !s_staNetif) return;
    uint8_t ihl = (uint8_t)((pkt[0] & 0x0F) * 4);
    if (ihl < 20 || ihl >= len) return;
    uint8_t proto = pkt[9];
    if (proto != 1 && proto != 6 && proto != 17) return;
    // Фрагменты IP обойти NAT не можем (порты только в первом) — пропускаем
    uint16_t fragOff = (uint16_t)(((pkt[6] & 0x1F) << 8) | pkt[7]);
    if (fragOff != 0) return;

    uint16_t phonePort;
    if (proto == 1) phonePort = (uint16_t)((pkt[ihl + 4] << 8) | pkt[ihl + 5]);
    else            phonePort = (uint16_t)((pkt[ihl] << 8) | pkt[ihl + 1]);
    uint32_t phoneIp = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                       ((uint32_t)pkt[14] << 8) | (uint32_t)pkt[15];
    uint32_t dstIp = ((uint32_t)pkt[16] << 24) | ((uint32_t)pkt[17] << 16) |
                     ((uint32_t)pkt[18] << 8) | (uint32_t)pkt[19];
    // cls guest — dst должен быть публичным (не 192.168.4.x и не broadcast/multicast)
    if ((dstIp & 0xFF000000) == 0xE0000000 || dstIp == 0xFFFFFFFF) return;

    NatEntry* e = natFindOrCreate(proto, phoneIp, phonePort);
    if (!e) { Serial.println("[NAT] table full"); return; }
    // Обновляем src IP на текущий STA-адрес
    uint32_t staIp = 0;
    if (s_staEspNetif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_staEspNetif, &info) == ESP_OK) staIp = info.ip.addr;
    }
    if (staIp == 0) { Serial.println("[NAT] STA IP not ready"); return; }

    // Строим сегмент: копия транспорта с подменённым src-портом
    uint16_t segLen = len - ihl;
    if (segLen > sizeof(s_upSeg)) segLen = sizeof(s_upSeg);
    memcpy(s_upSeg, pkt + ihl, segLen);
    if (proto == 1) {
        s_upSeg[4] = (uint8_t)(e->ext_port >> 8);
        s_upSeg[5] = (uint8_t)(e->ext_port & 0xFF);
    } else {
        s_upSeg[0] = (uint8_t)(e->ext_port >> 8);
        s_upSeg[1] = (uint8_t)(e->ext_port & 0xFF);
    }
    // Чексума сегмента с новым pseudo-header (src=STA, dst=Internet)
    uint8_t srcIpB[4] = { (uint8_t)(staIp >> 24), (uint8_t)(staIp >> 16),
                          (uint8_t)(staIp >> 8),  (uint8_t)staIp };
    uint8_t dstIpB[4] = { (uint8_t)(dstIp >> 24), (uint8_t)(dstIp >> 16),
                          (uint8_t)(dstIp >> 8),  (uint8_t)dstIp };
    if (proto == 1) {
        uint16_t csum = meshIpChecksum(s_upSeg, segLen);
        s_upSeg[2] = (uint8_t)(csum >> 8);
        s_upSeg[3] = (uint8_t)(csum & 0xFF);
    } else {
        uint16_t csum = meshIpTcpUdpChecksum(srcIpB, dstIpB, proto, s_upSeg, segLen);
        s_upSeg[6] = (uint8_t)(csum >> 8);
        s_upSeg[7] = (uint8_t)(csum & 0xFF);
    }
    e->last_ms = millis();

    s_upSegLen = segLen;
    s_upProto = proto;
    IP_ADDR4(&s_upSrc, srcIpB[0], srcIpB[1], srcIpB[2], srcIpB[3]);
    IP_ADDR4(&s_upDst, dstIpB[0], dstIpB[1], dstIpB[2], dstIpB[3]);
    tcpip_callback(natSendUpTcpip, NULL);
}

// ===================== Downstream: интернет → телефон =====================

static u8_t coordRawRecv(void* arg, struct raw_pcb* pcb, struct pbuf* p,
                         const ip_addr_t* src_ip) {
    (void)arg; (void)pcb; (void)src_ip;
    if (!p) return 0;
    uint8_t buf[MESH_IP_PKT_MAX];
    uint16_t copied = pbuf_copy_partial(p, buf, sizeof(buf), 0);
    if (copied < 20) { pbuf_free(p); return 0; }
    uint8_t ihl = (uint8_t)((buf[0] & 0x0F) * 4);
    uint8_t proto = buf[9];
    if (ihl < 20 || ihl >= copied) { pbuf_free(p); return 0; }
    uint16_t dstPort;
    if (proto == 1) {
        if (copied < ihl + 6) { pbuf_free(p); return 0; }
        dstPort = (uint16_t)((buf[ihl + 4] << 8) | buf[ihl + 5]);
    } else if (proto == 6 || proto == 17) {
        dstPort = (uint16_t)((buf[ihl + 2] << 8) | buf[ihl + 3]);
    } else {
        pbuf_free(p); return 0;
    }
    NatEntry* e = natFindByExt(proto, dstPort);
    if (!e) { pbuf_free(p); return 0; }   // не наш трафик — lwIP разберётся сам

    // Восстанавливаем адрес телефона
    buf[16] = (uint8_t)(e->phone_ip >> 24);
    buf[17] = (uint8_t)(e->phone_ip >> 16);
    buf[18] = (uint8_t)(e->phone_ip >> 8);
    buf[19] = (uint8_t)e->phone_ip;
    if (proto == 1) {
        buf[ihl + 4] = (uint8_t)(e->phone_port >> 8);
        buf[ihl + 5] = (uint8_t)(e->phone_port & 0xFF);
    } else {
        buf[ihl + 2] = (uint8_t)(e->phone_port >> 8);
        buf[ihl + 3] = (uint8_t)(e->phone_port & 0xFF);
    }
    // Чексумы: транспортный (src=Интернет, dst=телефон) и IP-заголовка
    uint16_t segLen = copied - ihl;
    if (proto == 1) {
        uint16_t csum = meshIpChecksum(buf + ihl, segLen);
        buf[ihl + 2] = (uint8_t)(csum >> 8);
        buf[ihl + 3] = (uint8_t)(csum & 0xFF);
    } else {
        uint16_t csum = meshIpTcpUdpChecksum(buf + 12, buf + 16, proto, buf + ihl, segLen);
        buf[ihl + 6] = (uint8_t)(csum >> 8);
        buf[ihl + 7] = (uint8_t)(csum & 0xFF);
    }
    uint16_t ipc = meshIpChecksum(buf, ihl);
    buf[10] = (uint8_t)(ipc >> 8);
    buf[11] = (uint8_t)(ipc & 0xFF);

    e->last_ms = millis();
    meshIpInject(buf, copied);
    pbuf_free(p);
    return 1;   // датаграмму забрали себе — в TCP/UDP-стек не пускаем
}

#endif // FEATURE_MESH_IP && MQTT_ENABLED