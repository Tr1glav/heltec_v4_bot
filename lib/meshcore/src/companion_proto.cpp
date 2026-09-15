#include "config.h"
#include "globals.h"
#include "companion.h"
#include "companion_internal.h"
#include "mesh.h"
#include "appconfig.h"
#include "display.h"

// ===== Разбор кадров телефонного приложения =====
// Выделено из companion.cpp: команды приложения, ответы на них и главный тик, который
// забирает кадры из очереди. Транспорт BLE, контакты и каналы остались в companion.cpp,
// общее между частями объявлено в companion_internal.h.

#if FEATURE_COMPANION

static uint8_t appVer = 0;      // версия протокола приложения из DEVICE_QUERY
static uint8_t out[MAX_FRAME_SIZE + 8];

static void handleFrame(const uint8_t* f, size_t len) {
    int i = 0;
    switch (f[0]) {
    case CMD_APP_START: {
        // [01][резерв 7][имя приложения] — версии протокола здесь нет
        out[i++] = RESP_CODE_SELF_INFO;
        out[i++] = ADV_TYPE_CHAT;
        out[i++] = (uint8_t)cfg.loraTx;
        out[i++] = 22;                       // предел мощности платы
        memcpy(&out[i], bot_pub, 32); i += 32;
        int32_t zero = 0;
        memcpy(&out[i], &zero, 4); i += 4;   // широта: не знаем
        memcpy(&out[i], &zero, 4); i += 4;   // долгота
        out[i++] = 0;                        // multi_acks
        out[i++] = 0;                        // advert_loc_policy
        out[i++] = 0;                        // телеметрия запрещена
        out[i++] = 1;                        // контакты добавляются вручную
        uint32_t freq = (uint32_t)(cfg.loraFreq * 1000.0f);
        memcpy(&out[i], &freq, 4); i += 4;
        uint32_t bw = (uint32_t)(cfg.loraBw * 1000.0f);
        memcpy(&out[i], &bw, 4); i += 4;
        out[i++] = (uint8_t)cfg.loraSf;
        out[i++] = (uint8_t)cfg.loraCr;
        size_t nlen = cfg.name.length();
        if (nlen > 32) nlen = 32;
        memcpy(&out[i], cfg.name.c_str(), nlen); i += nlen;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_DEVICE_QUERY: {
        // Версию протокола приложение сообщает именно здесь, вторым байтом. Читать её из
        // APP_START нельзя: там это поле зарезервировано, и мы получали ноль — то есть
        // считали приложение древним и слали ему кадр сообщения без качества связи и пути.
        if (len >= 2) appVer = f[1];
        out[i++] = RESP_CODE_DEVICE_INFO;
        out[i++] = COMPANION_VER_CODE;
        // В этом кадре — вместимость, а не текущее число: у оригинала здесь
        // MAX_CONTACTS / 2, то есть предел в единицах по два.
        out[i++] = COMPANION_MAX_CONTACTS / 2;
        out[i++] = MAX_CHANNELS;
        uint32_t pin = 0;
        memcpy(&out[i], &pin, 4); i += 4;
        memset(&out[i], 0, 12);
        strncpy((char*)&out[i], __DATE__, 11); i += 12;
        memset(&out[i], 0, 40);
        strncpy((char*)&out[i], "meshcore-fork", 39); i += 40;
        memset(&out[i], 0, 20);
        strncpy((char*)&out[i], FW_VERSION, 19); i += 20;
        out[i++] = 0;                        // ретрансляция выключена
        out[i++] = PATH_HASH_SIZE - 1;       // режим хэша пути: 0 — 1 байт, 1 — 2 байта
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_DEVICE_TIME: {
        out[i++] = RESP_CODE_CURR_TIME;
        uint32_t now = (uint32_t)time(NULL);
        memcpy(&out[i], &now, 4); i += 4;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SET_DEVICE_TIME: {
        if (len >= 5) {
            uint32_t epoch;
            memcpy(&epoch, &f[1], 4);
            if (epoch > (uint32_t)BUILD_UNIX_TIME) {
                struct timeval tv = { (time_t)epoch, 0 };
                settimeofday(&tv, NULL);
                timeSyncMs = millis();
            }
        }
        out[i++] = RESP_CODE_OK;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_CONTACTS: {
        // Необязательный аргумент «since»: приложение просит только изменившееся
        contactIterSince = 0;
        if (len >= 5) memcpy(&contactIterSince, &f[1], 4);
        out[i++] = RESP_CODE_CONTACTS_START;
        uint32_t count = contactCount;       // общее число, не отфильтрованное
        memcpy(&out[i], &count, 4); i += 4;
        sendFrameToApp(out, i);
        contactIterIdx = 0;                  // сами записи уходят из главного цикла
        contactIterNewest = 0;
        break;
    }
    case CMD_ADD_UPDATE_CONTACT: {
        // [09][ключ 32][тип][флаги][длина пути][путь 64][имя 32][время 4] + необязательные
        if (len < 1 + 32 + 3 + 64 + 32 + 4) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        const uint8_t* pub = &f[1];
        int idx = contactFind(pub);
        if (idx < 0) {
            if (contactCount >= COMPANION_MAX_CONTACTS) {
                out[i++] = RESP_CODE_ERR;
                out[i++] = ERR_CODE_TABLE_FULL;
                sendFrameToApp(out, i);
                break;
            }
            idx = contactCount++;
            memset(&contacts[idx], 0, sizeof(Contact));
        }
        Contact& c = contacts[idx];
        int o = 1;
        memcpy(c.pub, &f[o], 32); o += 32;
        c.type = f[o++];
        c.flags = f[o++];
        c.outPathLen = f[o++];
        memcpy(c.outPath, &f[o], 64); o += 64;
        memset(c.name, 0, sizeof(c.name));
        memcpy(c.name, &f[o], 31); o += 32;
        memcpy(&c.lastAdvert, &f[o], 4); o += 4;
        if (len >= (size_t)o + 8) {
            memcpy(&c.lat, &f[o], 4); o += 4;
            memcpy(&c.lon, &f[o], 4); o += 4;
        }
        c.lastmod = (uint32_t)time(NULL);
        contactTouch();
        out[i++] = RESP_CODE_OK;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_RESET_PATH: {
        // Забыть маршрут до узла: следующая отправка пойдёт флудом и путь построится заново
        int idx = (len >= 1 + 32) ? contactFind(&f[1]) : -1;
        if (idx < 0) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_NOT_FOUND;
        } else {
            contacts[idx].outPathLen = 0xFF;
            contactTouch();
            out[i++] = RESP_CODE_OK;
        }
        sendFrameToApp(out, i);
        break;
    }
    case CMD_REMOVE_CONTACT: {
        int idx = (len >= 33) ? contactFind(&f[1]) : -1;
        if (idx < 0) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_NOT_FOUND;
        } else {
            for (int k = idx; k + 1 < contactCount; k++) contacts[k] = contacts[k + 1];
            contactCount--;
            contactTouch();
            out[i++] = RESP_CODE_OK;
        }
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_ADVERT_PATH: {
        // [42][резерв][ключ 32] -> [22][когда слышали 4][длина пути][хэши ретрансляторов]
        int idx = (len >= 2 + 32) ? contactFind(&f[2]) : -1;
        if (idx < 0) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_NOT_FOUND;
        } else {
            const Contact& c = contacts[idx];
            uint8_t hops = c.advPathLen & 0x3F, hsize = (c.advPathLen >> 6) + 1;
            uint16_t bytes = (uint16_t)hops * hsize;
            // Длина пришла из NVS: запись могла остаться от другой версии, а в кадр
            // влезает ограниченно — лучше отдать пустой путь, чем выйти за буфер.
            if (bytes > sizeof(c.advPath)) bytes = 0;
            out[i++] = RESP_CODE_ADVERT_PATH;
            memcpy(&out[i], &c.lastmod, 4); i += 4;
            out[i++] = c.advPathLen;
            memcpy(&out[i], c.advPath, bytes); i += bytes;
        }
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_CONTACT_BY_KEY: {
        int idx = -1;
        if (len >= 2) {
            size_t klen = len - 1;           // приложение может прислать только начало ключа
            if (klen > 32) klen = 32;
            for (int k = 0; k < contactCount && idx < 0; k++)
                if (memcmp(contacts[k].pub, &f[1], klen) == 0) idx = k;
        }
        if (idx < 0) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_NOT_FOUND;
            sendFrameToApp(out, i);
        } else {
            sendFrameToApp(out, contactFrame(RESP_CODE_CONTACT, contacts[idx], out));
        }
        break;
    }
    case CMD_SET_CHANNEL: {
        // [32][номер][имя 32][ключ 16]
        if (len < 2 + 32 + 16) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        char nm[33];
        memset(nm, 0, sizeof(nm));
        memcpy(nm, &f[2], 32);
        // Каналы из настроек устройства приложению не отдаём: сохранить такую правку
        // мы не можем (их держит конфиг), и после перезагрузки она молча откатится.
        if (f[1] < appChanBase) {
            Serial.printf("[CH] канал %u задан настройками устройства, из приложения не меняем\n", f[1]);
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        int idx = channelSetSlot(f[1], nm, &f[2 + 32]);
        if (idx < 0) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_TABLE_FULL;
        } else {
            appChannelsSave();
            out[i++] = RESP_CODE_OK;
        }
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SET_ADVERT_NAME: {
        if (len < 2) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        String nn;
        for (size_t k = 1; k < len && k < 33; k++) nn += (char)f[k];
        cfg.name = nn;
        cfgSave();
        // Личность узла выводится из имени, поэтому после переименования её надо
        // пересчитать: иначе адверты уйдут со старым ключом.
        initAdvertIdentity();
        Serial.printf("[BLE] имя узла изменено на «%s»\n", cfg.name.c_str());
        out[i++] = RESP_CODE_OK;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_CHANNEL: {
        uint8_t idx = (len >= 2) ? f[1] : 0;
        // Приложение перебирает каналы подряд, пока не получит ошибку. Если отвечать
        // описанием канала на любой индекс, перебор не кончается никогда: телефон
        // бесконечно спрашивает следующий канал, а мы бесконечно отвечаем.
        if (idx >= numChannels) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_NOT_FOUND;
            sendFrameToApp(out, i);
            break;
        }
        out[i++] = RESP_CODE_CHANNEL_INFO;
        out[i++] = idx;
        memset(&out[i], 0, 32);
        strncpy((char*)&out[i], channels[idx].name, 31);
        i += 32;
        memcpy(&out[i], channels[idx].secret, 16);
        i += 16;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SEND_TXT_MSG: {
        // [02][тип][попытка][время 4][начало ключа 6][текст]
        if (len < 14) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        uint8_t txtType = f[1];
        const uint8_t* prefix = &f[7];       // приложение шлёт только первые 6 байт ключа
        int idx = -1;
        for (int k = 0; k < contactCount && idx < 0; k++)
            if (memcmp(contacts[k].pub, prefix, 6) == 0) idx = k;

        String text;
        for (size_t k = 13; k < len; k++) text += (char)f[k];
        if (text.length() > DM_TEXT_MAX) {
            text.remove(DM_TEXT_MAX);
            Serial.printf("[BLE] текст личного сообщения обрезан до %u байт\n",
                          (unsigned)DM_TEXT_MAX);
        }

        bool sent = false;
        if (idx >= 0 && txtType == 0 && text.length() > 0) {   // 0 — обычный текст
            uint8_t frame[256];
            int fl = buildPrivateTextFrame(contacts[idx].pub[0], contacts[idx].pub,
                                           text, frame, sizeof(frame));
            if (fl > 0) { floodSend(-1, frame, fl); sent = true; }
        }
        if (!sent) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = (idx < 0) ? ERR_CODE_NOT_FOUND : ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        // Метка нулевая: подтверждений доставки мы не отслеживаем, а ненулевая метка
        // заставила бы приложение ждать подтверждения, которое никогда не придёт.
        out[i++] = RESP_CODE_SENT;
        out[i++] = 1;                        // ушло флудом
        uint32_t tag = 0, est = 3000;
        memcpy(&out[i], &tag, 4); i += 4;
        memcpy(&out[i], &est, 4); i += 4;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SEND_CHANNEL_TXT_MSG: {
        // [03][00][канал][время 4][текст]
        if (len < 7) {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_ILLEGAL_ARG;
            sendFrameToApp(out, i);
            break;
        }
        uint8_t ch = f[2];
        // Кадр не оканчивается нулём, поэтому длину текста берём из длины кадра:
        // иначе в сообщение попадал бы мусор, оставшийся в буфере от прошлой команды.
        String text;
        for (size_t k = 7; k < len; k++) text += (char)f[k];
        bool sent = false;
        if (ch < numChannels && text.length() > 0) {
            uint8_t frame[256];
            int fl = buildGroupFrameFlood(ch, text, frame, sizeof(frame));
            if (fl > 0) { floodSend(ch, frame, fl); sent = true; }
        }
        if (sent) {
            // Оригинал отвечает одним байтом согласия. Мы отвечали кадром «отправлено»
            // с оценкой времени доставки — такого подтверждения приложение не ждёт, и
            // сообщение навсегда оставалось у него в состоянии «отправляется».
            out[i++] = RESP_CODE_OK;
        } else {
            out[i++] = RESP_CODE_ERR;
            out[i++] = ERR_CODE_NOT_FOUND;
        }
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SYNC_NEXT_MESSAGE: {
        if (msgCount == 0) {
            out[i++] = RESP_CODE_NO_MORE_MESSAGES;
            sendFrameToApp(out, i);
            break;
        }
        QueuedMsg& m = msgQueue[msgHead];
        // До версии 3 приложение не понимает полей качества связи — шлём короткий кадр
        if (appVer >= 3) {
            out[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
            out[i++] = (uint8_t)m.snr4;
            out[i++] = 0; out[i++] = 0;      // зарезервировано
        } else {
            out[i++] = RESP_CODE_CHANNEL_MSG_RECV;
        }
        out[i++] = m.channelIdx;
        out[i++] = m.pathLen;
        out[i++] = 0;                        // тип текста: обычный
        memcpy(&out[i], &m.ts, 4); i += 4;
        size_t tl = strlen(m.text);
        if (i + tl > MAX_FRAME_SIZE) tl = MAX_FRAME_SIZE - i;
        memcpy(&out[i], m.text, tl); i += tl;
        sendFrameToApp(out, i);
        msgHead = (msgHead + 1) % MSG_QUEUE_MAX;
        msgCount--;
        break;
    }
    case CMD_SEND_SELF_ADVERT: {
        // Второй байт: 1 — разослать по всей сети, иначе только ближайшим соседям
        sendAdvert((len >= 2 && f[1] == 1) ? ADV_ROUTE_FLOOD : ADV_ROUTE_DIRECT);
        out[i++] = RESP_CODE_OK;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_BATT_AND_STORAGE: {
        out[i++] = RESP_CODE_BATT_AND_STORAGE;
        uint16_t mv = (uint16_t)(batteryVoltage() * 1000.0f);
        memcpy(&out[i], &mv, 2); i += 2;
        uint32_t used = 0, total = 0;
        memcpy(&out[i], &used, 4); i += 4;
        memcpy(&out[i], &total, 4); i += 4;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_GET_DEFAULT_FLOOD_SCOPE: {
        // Областей рассылки у нас нет. В оригинале ответ из одного байта, без имени и
        // ключа, как раз и означает «область не задана»; приложение переспрашивало эту
        // команду по кругу, пока получало отказ.
        out[i++] = RESP_CODE_DEFAULT_FLOOD_SCOPE;
        sendFrameToApp(out, i);
        break;
    }
    case CMD_SET_FLOOD_SCOPE_KEY: {
        // Команда просит либо сбросить область, либо слать без неё — мы всегда так и
        // делаем, так что согласие здесь не обещает ничего, чего мы не выполняем.
        out[i++] = RESP_CODE_OK;
        sendFrameToApp(out, i);
        break;
    }
    default:
        Serial.printf("[BLE] команда %u пока не поддержана\n", f[0]);
        out[i++] = RESP_CODE_ERR;
        out[i++] = ERR_CODE_UNSUPPORTED_CMD;
        sendFrameToApp(out, i);
        break;
    }
}

void companionTick() {
    if (contactIterIdx >= 0 && blePaired) contactsIterStep();
    if (contactsDirty && millis() - contactsDirtyMs > CONTACTS_SAVE_DELAY_MS) contactsSave();

    static uint8_t frame[MAX_FRAME_SIZE];
    uint8_t n = 0;
    portENTER_CRITICAL(&inMux);
    if (inCount > 0) {
        n = inQueueLen[inHead];
        memcpy(frame, inQueue[inHead], n);
        inHead = (inHead + 1) % IN_QUEUE_MAX;
        inCount--;
    }
    uint32_t dropped = inDropped;
    inDropped = 0;
    portEXIT_CRITICAL(&inMux);

    if (dropped) Serial.printf("[BLE] очередь команд переполнена, потеряно кадров: %u\n", dropped);
    if (n == 0) return;
    Serial.printf("[BLE] <- команда %u, %u байт\n", frame[0], (unsigned)n);
    handleFrame(frame, n);
}

#endif // FEATURE_COMPANION
