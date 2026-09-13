#include "config.h"
#include "globals.h"
#include "companion.h"
#include "mesh.h"
#include "display.h"   // batteryVoltage для кадра о заряде

#ifdef COMPANION_NODE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <Preferences.h>

// --- протокол: коды из docs/companion_protocol.md оригинального MeshCore ---
#define CMD_APP_START              1
#define CMD_SEND_TXT_MSG           2
#define CMD_SEND_CHANNEL_TXT_MSG   3
#define CMD_GET_CONTACTS           4
#define CMD_GET_DEVICE_TIME        5
#define CMD_SET_DEVICE_TIME        6
#define CMD_SEND_SELF_ADVERT       7
#define CMD_SET_ADVERT_NAME        8
#define CMD_SYNC_NEXT_MESSAGE     10
#define CMD_SET_RADIO_PARAMS      11
#define CMD_GET_BATT_AND_STORAGE  20
#define CMD_DEVICE_QUERY          22
#define CMD_GET_CHANNEL           31
#define CMD_SET_CHANNEL           32
#define CMD_ADD_UPDATE_CONTACT     9
#define CMD_RESET_PATH            13
#define CMD_REMOVE_CONTACT        15
#define CMD_GET_CONTACT_BY_KEY    30
#define CMD_GET_ADVERT_PATH       42
#define CMD_SET_FLOOD_SCOPE_KEY   54
#define CMD_GET_DEFAULT_FLOOD_SCOPE 64

#define RESP_CODE_OK                0
#define RESP_CODE_ERR               1
#define RESP_CODE_CONTACTS_START    2
#define RESP_CODE_CONTACT           3
#define RESP_CODE_END_OF_CONTACTS   4
#define RESP_CODE_SELF_INFO         5
#define RESP_CODE_SENT              6
#define RESP_CODE_CURR_TIME         9
#define RESP_CODE_NO_MORE_MESSAGES 10
#define RESP_CODE_BATT_AND_STORAGE 12
#define RESP_CODE_DEVICE_INFO      13
#define RESP_CODE_CHANNEL_MSG_RECV 8    // формат для приложений до версии 3
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define RESP_CODE_CHANNEL_INFO     18
#define RESP_CODE_ADVERT_PATH      22
#define RESP_CODE_DEFAULT_FLOOD_SCOPE 28
#define PUSH_CODE_ADVERT         0x80    // знакомый узел объявился снова
#define PUSH_CODE_MSG_WAITING    0x83
#define PUSH_CODE_NEW_ADVERT     0x8A    // узел услышан впервые, кадр целиком

// Кадр ошибки в оригинале — два байта: RESP_CODE_ERR и причина
#define ERR_CODE_UNSUPPORTED_CMD    1
#define ERR_CODE_NOT_FOUND          2
#define ERR_CODE_TABLE_FULL         3
#define ERR_CODE_ILLEGAL_ARG        6

#define ADV_TYPE_CHAT              1
// Признаки в первом байте поля адверта. Порядок полей за ним строгий и зависит от
// признаков: координаты, два необязательных поля, и только потом имя.
#define ADV_LATLON_MASK         0x10
#define ADV_FEAT1_MASK          0x20
#define ADV_FEAT2_MASK          0x40
#define ADV_NAME_MASK           0x80
#define COMPANION_VER_CODE        13      // версия протокола, которую мы заявляем
#define COMPANION_FW_NAME         "meshcore-fork " FW_VERSION
#define MAX_FRAME_SIZE           176
#define MSG_QUEUE_MAX              8
// Кадры от приложения разбираются в главном цикле, а приходят пачкой из колбэка BLE:
// без очереди вторая команда затирала первую, и она пропадала молча.
#define IN_QUEUE_MAX               6

// UUID сервиса Nordic UART — именно по ним приложение ищет устройство
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // приложение -> прошивка
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // прошивка -> приложение

struct QueuedMsg {
    uint8_t channelIdx;
    uint8_t pathLen;
    int8_t snr4;          // SNR, умноженный на 4 — так требует протокол
    uint32_t ts;
    char text[100];
};

static BLEServer* bleServer = nullptr;
static BLECharacteristic* txChar = nullptr;
static volatile bool bleConnected = false;
static uint32_t blePin = 0;
static uint8_t appVer = 0;      // версия приложения из APP_START: от неё зависит формат кадров
// Экран с кодом убираем не по факту соединения, а только когда сопряжение состоялось:
// код нужен телефону именно в промежутке между подключением и вводом кода.
static volatile bool blePaired = false;
static QueuedMsg msgQueue[MSG_QUEUE_MAX];
static uint8_t msgHead = 0, msgCount = 0;
static uint8_t inQueue[IN_QUEUE_MAX][MAX_FRAME_SIZE];
static uint8_t inQueueLen[IN_QUEUE_MAX];
static volatile uint8_t inHead = 0, inCount = 0;
// Колбэк BLE выполняется в своей задаче, разбор — в главном цикле: индексы очереди
// трогаем только под блокировкой, иначе счётчик разъедется между ядрами.
static portMUX_TYPE inMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t inDropped = 0;
static uint8_t out[MAX_FRAME_SIZE + 8];

static void sendFrameToApp(const uint8_t* data, size_t len) {
    if (!bleConnected || txChar == nullptr || len == 0) return;
    Serial.printf("[BLE] -> код %u, %u байт\n", data[0], (unsigned)len);
    txChar->setValue((uint8_t*)data, len);
    txChar->notify();
}

// ===== Контакты =====
// Список узлов, чьи адверты мы слышали: приложение забирает его командой 4 и пополняет
// командой 9. Порядок полей в кадре повторяет оригинал байт в байт — приложение читает
// его по смещениям, и любая перестановка превращается в мусор на экране телефона.
struct Contact {
    uint8_t  pub[32];
    uint8_t  type;
    uint8_t  flags;
    uint8_t  outPathLen;        // 0xFF — путь неизвестен, отвечаем флудом
    uint8_t  outPath[64];
    char     name[32];
    uint32_t lastAdvert;        // по часам самого узла
    int32_t  lat, lon;
    uint32_t lastmod;           // по нашим часам: по нему приложение до-синхронизируется
    uint8_t  advPathLen;        // путь, которым пришёл последний адверт (байт длины как в эфире)
    uint8_t  advPath[64];       // сами хэши ретрансляторов — их приложение и показывает
};
static Contact contacts[COMPANION_MAX_CONTACTS];
static uint8_t contactCount = 0;
static bool contactsDirty = false;
static unsigned long contactsDirtyMs = 0;
static int contactIterIdx = -1;               // -1 — перебор списка не идёт
static int appChanBase = 0;                   // с этого номера идут каналы из приложения
static uint32_t contactIterSince = 0, contactIterNewest = 0;

static const char* COMPANION_NS = "companion";

static int contactFind(const uint8_t* pub) {
    for (int i = 0; i < contactCount; i++)
        if (memcmp(contacts[i].pub, pub, 32) == 0) return i;
    return -1;
}

// Запись в NVS откладываем: адверты приходят пачками, а каждая запись во флеш —
// это износ и задержка. Сохраняем, когда поток утих.
#define CONTACTS_SAVE_DELAY_MS 5000

static void contactsSave() {
    Preferences p;
    if (!p.begin(COMPANION_NS, false)) return;
    p.putUChar("ccount", contactCount);
    // Ключ с номером: состав записи менялся, и чтение старой длины дало бы мусор в списке
    if (contactCount > 0) p.putBytes("contacts2", contacts, sizeof(Contact) * contactCount);
    p.end();
    contactsDirty = false;
    Serial.printf("[BLE] контакты сохранены: %u\n", contactCount);
}

static void contactsLoad() {
    Preferences p;
    // На запись, как и конфиг: в режиме только для чтения несуществующее пространство
    // имён даёт ошибку в журнале при каждом первом запуске устройства.
    if (!p.begin(COMPANION_NS, false)) return;
    uint8_t n = p.getUChar("ccount", 0);
    if (n > COMPANION_MAX_CONTACTS) n = COMPANION_MAX_CONTACTS;
    // Счётчик и записи — разные ключи NVS, и после смены формата счётчик может пережить
    // данные. Если прочиталось не столько, сколько обещано, список считаем пустым:
    // иначе приложение получило бы контакты из мусора.
    size_t need = sizeof(Contact) * n;
    if (n > 0 && p.getBytes("contacts2", contacts, need) != need) {
        Serial.println("[BLE] записи контактов не совпали с счётчиком, начинаем с пустого списка");
        n = 0;
    }
    contactCount = n;
    p.end();
    Serial.printf("[BLE] контактов загружено: %u\n", contactCount);
}

static void contactTouch() { contactsDirty = true; contactsDirtyMs = millis(); }

static int contactFrame(uint8_t code, const Contact& c, uint8_t* buf) {
    int i = 0;
    buf[i++] = code;
    memcpy(&buf[i], c.pub, 32);      i += 32;
    buf[i++] = c.type;
    buf[i++] = c.flags;
    buf[i++] = c.outPathLen;
    memcpy(&buf[i], c.outPath, 64);  i += 64;
    memset(&buf[i], 0, 32);
    strncpy((char*)&buf[i], c.name, 31); i += 32;
    memcpy(&buf[i], &c.lastAdvert, 4); i += 4;
    memcpy(&buf[i], &c.lat, 4);        i += 4;
    memcpy(&buf[i], &c.lon, 4);        i += 4;
    memcpy(&buf[i], &c.lastmod, 4);    i += 4;
    return i;                          // 148 байт, в кадр (176) помещается
}

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        size_t n = c->getLength();
        if (n == 0 || n > MAX_FRAME_SIZE) return;
        // Писать в эту характеристику разрешено только по зашифрованной связи,
        // так что пришедший кадр сам по себе доказывает состоявшееся сопряжение
        blePaired = true;
        portENTER_CRITICAL(&inMux);
        if (inCount >= IN_QUEUE_MAX) {
            inDropped++;                 // очередь переполнена — кадр потерян, но об этом узнаем
        } else {
            uint8_t slot = (inHead + inCount) % IN_QUEUE_MAX;
            memcpy(inQueue[slot], c->getData(), n);
            inQueueLen[slot] = (uint8_t)n;
            inCount++;
        }
        portEXIT_CRITICAL(&inMux);
    }
};

// Приложение MeshCore подключается только к сопряжённому устройству: обе характеристики
// оригинала доступны исключительно по зашифрованной связи с подтверждением личности
// (ESP_GATT_PERM_*_ENC_MITM). Без этого телефон начинает сопряжение, не получает ответа
// и остаётся в состоянии «подключаюсь» — связь при этом даже не устанавливается.
class SecCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return blePin; }
    void onPassKeyNotify(uint32_t key) override { Serial.printf("[BLE] код сопряжения %u\n", key); }
    bool onConfirmPIN(uint32_t) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        if (cmpl.success) {
            blePaired = true;
            Serial.println("[BLE] сопряжение выполнено");
        } else {
            Serial.printf("[BLE] сопряжение не удалось (причина %u)\n", cmpl.fail_reason);
            if (bleServer) bleServer->disconnect(bleServer->getConnId());
        }
    }
};

class SrvCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*) override { bleConnected = true; Serial.println("[BLE] приложение подключилось"); }
    void onDisconnect(BLEServer* s) override {
        bleConnected = false;
        blePaired = false;
        // Иначе при следующем подключении список контактов поедет с середины,
        // без начального кадра, и приложение примет обрывок за весь список.
        contactIterIdx = -1;
        Serial.println("[BLE] приложение отключилось");
        s->startAdvertising();
    }
};

// Каналы, заведённые из приложения, живут в NVS рядом с контактами: иначе после
// перезагрузки приложение видело бы пустой список и вступать в канал пришлось бы заново.
// Номер ячейки не храним: состав каналов из настроек меняется (сегодня добавился
// приватный), и сохранённый номер попал бы на встроенный канал и затёр его. Храним имя
// с ключом, а при загрузке дописываем в конец списка.
struct AppChanRec { uint8_t idx; char name[33]; uint8_t key[16]; };

void appChannelsSave() {
    AppChanRec recs[MAX_CHANNELS];
    uint8_t n = 0;
    for (int k = 0; k < numChannels && n < MAX_CHANNELS; k++) {
        if (k < appChanBase) continue;            // каналы из настроек хранит сам конфиг
        recs[n].idx = 0;                          // поле осталось от прежнего формата
        memset(recs[n].name, 0, sizeof(recs[n].name));
        strncpy(recs[n].name, channels[k].name, 32);
        memcpy(recs[n].key, channels[k].secret, 16);
        n++;
    }
    Preferences p;
    if (!p.begin(COMPANION_NS, false)) return;
    p.putUChar("chcount", n);
    if (n > 0) p.putBytes("chans", recs, sizeof(AppChanRec) * n);
    p.end();
    Serial.printf("[CH] каналов из приложения сохранено: %u\n", n);
}

static void appChannelsLoad() {
    Preferences p;
    if (!p.begin(COMPANION_NS, false)) return;
    uint8_t n = p.getUChar("chcount", 0);
    if (n > MAX_CHANNELS) n = MAX_CHANNELS;
    AppChanRec recs[MAX_CHANNELS];
    if (n > 0) p.getBytes("chans", recs, sizeof(AppChanRec) * n);
    p.end();
    for (uint8_t k = 0; k < n; k++) {
        recs[k].name[32] = 0;
        int at = findChannelByName(recs[k].name);          // уже есть — обновим ключ
        channelSetSlot(at >= 0 ? at : numChannels, recs[k].name, recs[k].key);
    }
}

void companionBegin() {
    appChanBase = numChannels;      // всё, что дальше, добавлено из приложения
    contactsLoad();
    appChannelsLoad();
    String name = cfgReady() ? cfg.name : String("MeshCore");
    // Код свой на каждый запуск: подсмотреть его можно только у экрана самого устройства
    blePin = 100000 + (esp_random() % 900000);
    BLEDevice::init(name.c_str());
    BLEDevice::setSecurityCallbacks(new SecCallbacks());
    // Без этого ATT MTU остаётся 23 байта: в уведомление влезает 20, а наши кадры
    // длиннее (только SELF_INFO около 70). Приложение получало обрезанный ответ и
    // ждало продолжения — со стороны это выглядит как «подключается и висит».
    BLEDevice::setMTU(MAX_FRAME_SIZE);
    // setStaticPIN сам выставляет режим «только отображаем код» и SC_ONLY, поэтому
    // требуемый режим проверки подлинности задаём после него, как это делает оригинал
    BLESecurity sec;
    sec.setStaticPIN(blePin);
    sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new SrvCallbacks());
    BLEService* svc = bleServer->createService(NUS_SERVICE);
    BLECharacteristic* rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE);
    rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
    rx->setCallbacks(new RxCallbacks());
    txChar = svc->createCharacteristic(
        NUS_TX, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    txChar->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
    txChar->addDescriptor(new BLE2902());
    svc->start();
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.printf("[BLE] компаньон «%s» ждёт подключения, код сопряжения %u\n",
                  name.c_str(), blePin);
}

uint32_t companionBlePin() { return blePin; }
bool companionBleLinked() { return blePaired; }

void companionOnChannelText(int channelIdx, const String& text, float snr, uint8_t pathLen,
                            bool notify) {
    if (msgCount >= MSG_QUEUE_MAX) {   // очередь полна — вытесняем самое старое
        msgHead = (msgHead + 1) % MSG_QUEUE_MAX;
        msgCount--;
    }
    QueuedMsg& m = msgQueue[(msgHead + msgCount) % MSG_QUEUE_MAX];
    m.channelIdx = (uint8_t)channelIdx;
    m.pathLen = pathLen;
    long s4 = lround(snr * 4.0f);
    m.snr4 = (int8_t)(s4 < -128 ? -128 : (s4 > 127 ? 127 : s4));
    m.ts = (uint32_t)time(NULL);
    if (text.length() >= sizeof(m.text))
        Serial.printf("[BLE] сообщение обрезано: %u -> %u байт\n",
                      text.length(), (unsigned)sizeof(m.text) - 1);
    strlcpy(m.text, text.c_str(), sizeof(m.text));
    msgCount++;
    // Сигнал «есть новое» рождает в телефоне уведомление. Для того, что устройство
    // отправило само, это уведомление о собственном действии — кладём в очередь молча,
    // и приложение покажет сообщение при ближайшей синхронизации.
    if (!notify) return;
    uint8_t push = PUSH_CODE_MSG_WAITING;
    sendFrameToApp(&push, 1);   // приложение заберёт сообщение командой 10
}

// По одному контакту за проход главного цикла: пачка кадров подряд переполняет
// очередь уведомлений BLE, и приложение получает обрывки списка.
static void contactsIterStep() {
    uint8_t buf[160];
    while (contactIterIdx < contactCount) {
        Contact& c = contacts[contactIterIdx++];
        if (c.lastmod <= contactIterSince) continue;      // приложение уже знает эту запись
        if (c.lastmod > contactIterNewest) contactIterNewest = c.lastmod;
        sendFrameToApp(buf, contactFrame(RESP_CODE_CONTACT, c, buf));
        return;
    }
    buf[0] = RESP_CODE_END_OF_CONTACTS;
    memcpy(&buf[1], &contactIterNewest, 4);
    sendFrameToApp(buf, 5);
    contactIterIdx = -1;
}

void companionOnAdvert(const uint8_t* pub, const uint8_t* app, int applen,
                       uint8_t pathLen, const uint8_t* path) {
    uint8_t advFlags = applen > 0 ? app[0] : 0;
    uint8_t type = advFlags & 0x0F;
    // Имя лежит не на втором байте, а после всех присутствующих полей. Раньше мы брали
    // его со смещения 1 всегда, и у узлов с координатами в начало имени попадали байты
    // широты и долготы — в приложении это выглядело как мусор перед именем.
    int o = 1;
    int32_t lat = 0, lon = 0;
    bool hasLoc = false;
    if (advFlags & ADV_LATLON_MASK) {
        if (o + 8 <= applen) {
            memcpy(&lat, &app[o], 4);
            memcpy(&lon, &app[o + 4], 4);
            hasLoc = true;
        }
        o += 8;
    }
    if (advFlags & ADV_FEAT1_MASK) o += 2;
    if (advFlags & ADV_FEAT2_MASK) o += 2;
    char nm[32];
    memset(nm, 0, sizeof(nm));
    if ((advFlags & ADV_NAME_MASK) && o < applen) {
        int n = applen - o;
        if (n > 31) n = 31;
        memcpy(nm, &app[o], n);
    }

    int idx = contactFind(pub);
    bool isNew = (idx < 0);
    if (isNew) {
        if (contactCount >= COMPANION_MAX_CONTACTS) {
            idx = 0;                                      // вытесняем самого давнего
            for (int k = 1; k < contactCount; k++)
                if (contacts[k].lastmod < contacts[idx].lastmod) idx = k;
        } else {
            idx = contactCount++;
        }
        memset(&contacts[idx], 0, sizeof(Contact));
        memcpy(contacts[idx].pub, pub, 32);
        contacts[idx].outPathLen = 0xFF;                  // пути не знаем — только флудом
    }
    Contact& c = contacts[idx];
    c.type  = type;
    // c.flags — это флаги контакта в приложении (например «избранный»), а не признаки
    // адверта: их выставляет само приложение командой 9, и затирать их нельзя.
    if (nm[0]) strncpy(c.name, nm, sizeof(c.name) - 1);
    if (hasLoc) { c.lat = lat; c.lon = lon; }
    c.lastAdvert = c.lastmod = (uint32_t)time(NULL);
    // Путь запоминаем как пришёл: приложение спрашивает его командой 42, чтобы показать,
    // через каких соседей слышно узел.
    uint8_t hops = pathLen & 0x3F, hsize = (pathLen >> 6) + 1;
    uint16_t bytes = (uint16_t)hops * hsize;
    if (path != nullptr && bytes <= sizeof(c.advPath)) {
        c.advPathLen = pathLen;
        memcpy(c.advPath, path, bytes);
    }
    contactTouch();
    Serial.printf("[ADV] %s контакт <%02X> %s\n", isNew ? "новый" : "известный", pub[0], c.name);

    if (!blePaired) return;
    uint8_t buf[160];
    if (isNew) {
        sendFrameToApp(buf, contactFrame(PUSH_CODE_NEW_ADVERT, c, buf));
    } else {
        buf[0] = PUSH_CODE_ADVERT;
        memcpy(&buf[1], c.pub, 32);
        sendFrameToApp(buf, 33);
    }
}

void appChannelsSave();   // определена ниже, вызывается при вступлении в канал

static void handleFrame(const uint8_t* f, size_t len) {
    int i = 0;
    switch (f[0]) {
    case CMD_APP_START: {
        // [01][версия приложения][6 байт резерва][имя приложения]
        appVer = (len >= 2) ? f[1] : 0;
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
        if (text.length() > 96)   // столько влезает в кадр личного сообщения
            Serial.printf("[BLE] текст личного сообщения обрезан: %u -> 96 байт\n", text.length());

        bool sent = false;
        if (idx >= 0 && txtType == 0 && text.length() > 0) {   // 0 — обычный текст
            uint8_t frame[192];
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
#endif
