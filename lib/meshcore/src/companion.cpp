#include "config.h"
#include "globals.h"
#include "companion.h"
#include "companion_internal.h"
#include "mesh.h"
#include "display.h"   // batteryVoltage для кадра о заряде

#ifdef COMPANION_NODE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <Preferences.h>


static BLEServer* bleServer = nullptr;
static BLECharacteristic* txChar = nullptr;
static volatile bool bleConnected = false;
static uint32_t blePin = 0;
// Экран с кодом убираем не по факту соединения, а только когда сопряжение состоялось:
// код нужен телефону именно в промежутке между подключением и вводом кода.
volatile bool blePaired = false;
QueuedMsg msgQueue[MSG_QUEUE_MAX];
uint8_t msgHead = 0, msgCount = 0;
uint8_t inQueue[IN_QUEUE_MAX][MAX_FRAME_SIZE];
uint8_t inQueueLen[IN_QUEUE_MAX];
volatile uint8_t inHead = 0, inCount = 0;
// Колбэк BLE выполняется в своей задаче, разбор — в главном цикле: индексы очереди
// трогаем только под блокировкой, иначе счётчик разъедется между ядрами.
portMUX_TYPE inMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t inDropped = 0;

void sendFrameToApp(const uint8_t* data, size_t len) {
    if (!bleConnected || txChar == nullptr || len == 0) return;
    Serial.printf("[BLE] -> код %u, %u байт\n", data[0], (unsigned)len);
    txChar->setValue((uint8_t*)data, len);
    txChar->notify();
}

// ===== Контакты =====
// Список узлов, чьи адверты мы слышали: приложение забирает его командой 4 и пополняет
// командой 9. Порядок полей в кадре повторяет оригинал байт в байт — приложение читает
// его по смещениям, и любая перестановка превращается в мусор на экране телефона.
Contact contacts[COMPANION_MAX_CONTACTS];
uint8_t contactCount = 0;
bool contactsDirty = false;
unsigned long contactsDirtyMs = 0;
int contactIterIdx = -1;               // -1 — перебор списка не идёт
int appChanBase = 0;                   // с этого номера идут каналы из приложения
uint32_t contactIterSince = 0, contactIterNewest = 0;

static const char* COMPANION_NS = "companion";

int contactFind(const uint8_t* pub) {
    for (int i = 0; i < contactCount; i++)
        if (memcmp(contacts[i].pub, pub, 32) == 0) return i;
    return -1;
}


#define CONTACTS_FILE "/contacts.bin"

// Контакты живут файлом, а не записью в NVS: раздел nvs — это 20 КБ на всё вместе с
// настройками, а список на 200 узлов занимает уже сорок с лишним килобайт.
void contactsSave() {
    File f = LittleFS.open(CONTACTS_FILE, "w");
    if (!f) { Serial.println("[BLE] файл контактов не открылся на запись"); return; }
    f.write(&contactCount, 1);
    size_t need = sizeof(Contact) * contactCount;
    bool ok = (contactCount == 0) || (f.write((const uint8_t*)contacts, need) == need);
    f.close();
    if (!ok) { Serial.println("[BLE] список контактов записан не полностью"); return; }
    contactsDirty = false;
    Serial.printf("[BLE] контакты сохранены: %u\n", contactCount);
}

static void contactsLoad() {
    contactCount = 0;
    File f = LittleFS.open(CONTACTS_FILE, "r");
    if (!f) { Serial.println("[BLE] списка контактов ещё нет"); return; }
    uint8_t n = 0;
    if (f.read(&n, 1) != 1) { f.close(); return; }
    if (n > COMPANION_MAX_CONTACTS) n = COMPANION_MAX_CONTACTS;
    size_t need = sizeof(Contact) * n;
    // Прочиталось меньше обещанного — файл оборван или от другой версии записи:
    // лучше пустой список, чем контакты из мусора.
    if (n > 0 && f.read((uint8_t*)contacts, need) != (int)need) {
        Serial.println("[BLE] файл контактов повреждён, начинаем с пустого списка");
        n = 0;
    }
    f.close();
    contactCount = n;
    Serial.printf("[BLE] контактов загружено: %u\n", contactCount);
}

void contactTouch() { contactsDirty = true; contactsDirtyMs = millis(); }

int contactFrame(uint8_t code, const Contact& c, uint8_t* buf) {
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
    // Интервал рекламы в единицах по 0.625 мс: 800–1200 мс вместо частого объявления.
    // Телефон всё равно находит устройство за пару секунд, а радио большую часть
    // времени молчит — на аккумуляторе это несколько миллиампер разницы.
    adv->setMinInterval(1280);
    adv->setMaxInterval(1920);
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
void contactsIterStep() {
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

#endif
