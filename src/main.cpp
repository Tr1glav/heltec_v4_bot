// MeshCore bot/listener + sensor node. setup()/loop() only —
// логика вынесена в модули (lib/meshcore): radio, mesh, ota, mqtt, display.
// Вручную поддерживаемый файл; никакой генерации из parts/.
#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "radio.h"
#include "mesh.h"
#include "display.h"
#include "button.h"
#include "sensor_tasks.h"
#include "coordinator_tasks.h"
#include "ota.h"
#include "mqtt.h"
#include "fwupdate.h"
#include "companion.h"

void initSystemClock() {
    struct timeval tv;
    tv.tv_sec = BUILD_UNIX_TIME;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    // Локальное время считаем вручную: UTC + фиксированное смещение.
    time_t local = (time_t)BUILD_UNIX_TIME + (time_t)cfg.tzOffset * 3600;
    struct tm tm_now;
    gmtime_r(&local, &tm_now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
    Serial.printf("[RTC] SysTime set from host: %s (%s)\n", buf, LOCAL_TZ);
    Serial.printf("[RTC] epoch=%lld\n", (long long)time(NULL));
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== MESHCORE LISTENER ===\n");
    Serial.printf("[FW] %s\n", fwMarker);

    // Настройки читаются до радио и каналов: от них зависят имя узла и ключи каналов
    if (!cfgLoad()) {
        Serial.println("\n[CFG] устройство не настроено — наберите help в этой консоли");
    }
    cfgPrint(false);
    
    initSystemClock();
    
    // ===== ПИТАНИЕ ПЕРИФЕРИИ (VEXT) =====
    #if HAS_OLED && defined(VEXT_PIN)
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, cfg.vextOn ? HIGH : LOW);
    delay(300);
    #endif
    
    // ===== FEM (усилитель KCT8103L на V4) =====
    #if HAS_FEM
    Serial.println("Init FEM...");
    pinMode(FEM_VCC_PIN, OUTPUT);
    pinMode(FEM_EN_PIN, OUTPUT);
    pinMode(FEM_TX_PIN, OUTPUT);
    digitalWrite(FEM_VCC_PIN, HIGH);
    delay(10);
    digitalWrite(FEM_EN_PIN, HIGH);
    delay(10);
    digitalWrite(FEM_TX_PIN, LOW);  // RX
    delay(10);
    Serial.println("FEM OK");
    #endif
    
    // ===== OLED =====
    #if HAS_OLED
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, LOW);
    delay(10);
    digitalWrite(OLED_RESET, HIGH);
    delay(200);

    #if defined(SDA_PIN) && defined(SCL_PIN)
    Wire.begin(SDA_PIN, SCL_PIN);
    #else
    Wire.begin();
    #endif
    Wire.setClock(100000);   // 400k часть OLED-панелей V4 "мусорит" — снижаем

    // Сканер I2C: некоторые экземпляры Heltec V4 живут на 0x3D, а не 0x3C.
    // Полоски/мусор на экране часто = неверный адрес или ранний инит.
    uint8_t oledAddr = SCREEN_ADDRESS;
    bool addrFound = false;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] device found at 0x%02X\n", a);
            if (a == 0x3C || a == 0x3D) {
                oledAddr = a;
                addrFound = true;
            }
        }
    }
    if (!addrFound) {
        Serial.printf("[OLED] no 0x3C/0x3D found, defaulting to 0x%02X\n", oledAddr);
    } else {
        Serial.printf("[OLED] address = 0x%02X\n", oledAddr);
    }

    if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
        // пробуем альтернативный адрес
        uint8_t alt = (oledAddr == 0x3C) ? 0x3D : 0x3C;
        if (!display.begin(SSD1306_SWITCHCAPVCC, alt)) {
            Serial.println("Display FAILED!");
            while (1) delay(1000);
        } else {
            Serial.printf("OLED ok at 0x%02X (alt)\n", alt);
        }
    } else {
        Serial.printf("OLED ok at 0x%02X\n", oledAddr);
    }

    // Принудительно очищаем 2 раза и снимаем dim — уже фактически
    // устраняет "полоски" начального мусора на SSD1306
    display.dim(false);
    display.clearDisplay();
    display.display();
    delay(50);
    display.clearDisplay();
    display.display();
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("MeshCore");
    display.println("V4.3 init...");
    display.display();
    #endif
    
    #if BUTTON_PIN >= 0
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    #endif
    
    // ===== SPI =====
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    // ===== RESET =====
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(20);
    digitalWrite(LORA_RST, HIGH);
    delay(100);
    
    // ===== LORA =====
    if (!initLoRa()) {
        #if HAS_OLED
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("LoRa ERROR!");
        display.display();
        #endif
        while (1) {
            delay(1000);
            Serial.println("LoRa init FAILED");
        }
    }
    
    deriveChannels();
    initAdvertIdentity();

#ifdef MQTT_ENABLED
    loadTxChannel();
    // Сеть НЕ блокируем: WiFi/MQTT поднимаются фоном в loop (tickRetryConnections).
    // Радио начинает слушать сразу, без задержки на TCP/а-коннект.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // без modem-sleep: меньше задержка отвёта
    // Дамп РЕАЛЬНОЙ таблицы разделов с устройства — для отладки LittleFS.
    // Если spiffs нет/другой офсет — mesh OTA работать не будет.
    {
        slog("[PART] flash chip size: %u KB\n", ESP.getFlashChipSize() / 1024);
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                         ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t* p = esp_partition_get(it);
            slog("[PART] %-10s type=%d sub=%d off=0x%06X size=%uK\n",
                 p->label, (int)p->type, (int)p->subtype,
                 (unsigned)p->address, p->size / 1024);
        }
        if (it) esp_partition_iterator_release(it);
    }
    // хранилище /ota.bin для mesh OTA сенсоров (partition "spiffs", 1.5 MB)
    if (LittleFS.begin(true)) {
        slog("[LITTLEFS] OK\n");
        // Проверка записи вынесена на /selftest: она пишет во флеш, а результат нужен
        // только при разборе проблем, не при каждом старте.
    } else {
        slog("[LITTLEFS] FAILED (begin) — mesh OTA недоступен\n");
    }
    if (LittleFS.exists("/ota.bin")) {
        otaInspectStoredFw();
        slog("[OTA] /ota.bin: %u байт (mesh OTA ready=%d)\n",
             (unsigned)otaFwSize, (int)otaFwReady);
    }
    setupMQTT();            // только конфиг (префиксы/сервер/коллбэк)
    setupOtaServer();       // HTTP OTA на :3232 (обновление прошивки по WiFi)
    #endif

    radio.startReceive();
    isListening = true;
    lastDirectAdvertMs = lastFloodAdvertMs = millis();

    #ifdef COMPANION_NODE
    // Список контактов лежит файлом, поэтому файловую систему монтируем до BLE.
    // У координатора она уже смонтирована выше — там в ней живёт образ для mesh OTA.
    #ifndef MQTT_ENABLED
    if (!LittleFS.begin(true)) Serial.println("[FS] не смонтировалась — контакты не сохранятся");
    #endif
    companionBegin();   // BLE поднимаем после радио: приложение может подключиться сразу
    #endif

    #ifdef SENSOR_NODE
    // Узлы работают от аккумулятора: 80 МГц вместо 240 экономят 20–30 мА, а BLE и LoRa
    // на этой частоте штатны. На время прошивки по воздуху частота поднимается обратно.
    // Пересчитывать скорость порта не нужно: на этих платах консоль идёт по нативному
    // USB, где скорость номинальная, а для UART ядро само правит делитель при смене
    // частоты.
    setCpuFrequencyMhz(CPU_MHZ_IDLE);
    Serial.printf("[PWR] частота процессора %d МГц\n", CPU_MHZ_IDLE);
    #endif
    
    #if HAS_OLED
    display.clearDisplay();
    // Стартовая заставка: только имя устройства по центру экрана (128x64).
    display.setTextSize(2);                     // 12x16 симв.
    const char* splash = "MeshCore";
    display.setCursor((128 - (int)strlen(splash) * 12) / 2, (64 - 16) / 2);
    display.print(splash);
    display.setTextSize(1);
    display.display();
    #endif
    
    // Яркость применяем в самом конце инициализации, а не сразу после display.begin():
    // ранняя запись в регистры панель гасила. Проверено: контраст выше штатного 0xCF
    // прибавки не даёт, поэтому настройка полезна в основном для затемнения.
    #if HAS_OLED
    display.setBrightness((uint8_t)cfg.dispBri);
    #endif

    Serial.printf("Listening on %s...\n", channelListStr().c_str());
}

void loop() {
    #ifdef MQTT_ENABLED
    otaServer.handleClient();   // HTTP OTA: принимаем реквесты не блокируя радио
    otaBotTick();               // mesh OTA: таймауты повтора чанков

    // ===== MQTT RECONNECT (неблокирующий, раз в 5 с) =====
    if (millis() - lastMqttReconnectMs > MQTT_RECONNECT_INTERVAL_MS) {
        lastMqttReconnectMs = millis();
        tickRetryConnections();
    }
    if (mqttConnected) mqtt.loop();
    #endif

    cfgConsoleTick();   // настройка через USB-консоль, не блокирует радио

    // ===== ADVERT (периодический) =====
    // без настроек в эфир не выходим: имя узла пустое, каналов нет
    if (isListening && !otaFastMode && cfgReady()) {
        if (!advertBootSent && millis() > 6000) {   // стартовый beacon
            advertBootSent = true;
            sendAdvert(ADV_ROUTE_DIRECT);
        }
        if (millis() - lastDirectAdvertMs >= ADVERT_PERIOD_MS) {
            lastDirectAdvertMs = millis();
            sendAdvert(ADV_ROUTE_DIRECT);
        }
        if (millis() - lastFloodAdvertMs >= ADVERT_FLOOD_PERIOD_MS) {
            lastFloodAdvertMs = millis();
            sendAdvert(ADV_ROUTE_FLOOD);
        }
    }

    radioRxTick();   // приём из эфира: опрос радио, разбор кадров, поддержание приёмника

    statusScreenTick();   // статус на экране раз в полсекунды

    #ifdef MQTT_ENABLED
    coordinatorTasksTick();   // MQTT, время, доступность узлов, проверка обновлений
    #endif

    // Sensor node: button = trigger ("button"), hello = heartbeat раз в N минут
    #if FEATURE_SENSOR
    sensorTasksTick();   // задачи узла: heartbeat, кнопка, экран, частота, BLE
    #endif

    // во время mesh OTA кадры пачки идут каждые ~40 мс — опрашиваем радио чаще
    delay(otaFastMode ? 1 : 10);
}
