# MeshCore listener bot (Heltec V4 / multi-board)

Прошивка на базе [MeshCore](https://github.com/meshcore-dev/MeshCore) для
ESP32-S3 + SX1262: слушает каналы `#public` и `#connections`, отвечает на
`/ping` (групповые и личные TXT_MSG), раздаёт адверты своей сборки и
показывает состояние на OLED (включая кириллицу).

## Оглавление

- [Структура проекта](#структура-проекта)
- [Сборка и прошивка](#сборка-и-прошивка)
- [Поддерживаемые платы / env'ы](#поддерживаемые-платы--envы)
- [Как добавить свою плату](#как-добавить-свою-плату)
- [Русский шрифт на OLED](#русский-шрифт-на-oled)
- [Параметры радио и каналы](#параметры-радио-и-каналы)
- [MQTT / Home Assistant](#mqtt--home-assistant)
- [Драйвер OLED (SH1106 vs SSD1306)](#драйвер-oled-sh1106-vs-ssd1306)

## Структура проекта

```
boards/                        кастомные определения плат (Heltec WiFi LoRa 32 V4)
variants/                      кастомные variants (pins_arduino.h)
lib/
  target.h                     хук сборки MeshCore (RADIO_CLASS/WRAPPER_CLASS/...)
  HeltecV4Board.h              базовый класс платы MeshCore
  LoRaFEMControl.h             заглушка SPDT/FEM-контроллера
scripts/
  gen_build_time.py            генерирует BUILD_UNIX_TIME (часы из момента сборки)
  gen_cyrillic_font.py         генерирует 6x8 растр кириллицы (CP866)
src/
  board_config.h               отображение build_flags -> макросы приложения,
                               выбор дисплея, инстанс display
  main.cpp                     логика бота
  ed25519/                     исходники ed25519 (Nightcracker) — key_exchange,
                               sign/verify, SHA-512; используются для X25519
                               shared-secret (личные сообщения) и проверки
                               подписей ADVERT-пакетов
  sysoled.h                    компактный I2C-драйвер SH1106 (свой, без Adafruit)
  cyrillic.h                   UTF-8 -> CP866 декодер + глифы через хук write()
  cyrillic_glyphs.h            сгенерированная таблица 6x8 (114 глифов)
  _preview.png                 превью шрифта (генерируется скриптом)
platformio.ini                 все env'ы и флаги плат
```

## Сборка и прошивка

```bash
# прошивка сползает на USB-порт (PlatformIO сам найдёт первый COM/ttyACM)
pio run -t upload -e heltec_v4_3

# монитор
pio device monitor -e heltec_v4_3
```

Нужен PlatformIO >= 6. Зависимости тянутся из кэша; при чистом клоне дёрнется
ocё git-кэш (RadioLib 6d89348, MeshCore) — на машине поддерживается HTTPS к
GitHub.

## Поддерживаемые платы / env'ы

| env | плата | OLED | FEM | смысл |
|-----|-------|------|-----|-------|
| `heltec_v4_3` | Heltec WiFi LoRa 32 V4.3 | SH1106 (встроенный драйвер) | KCT8103L | основной |
| `heltec_v4_3_ssd1306` | та же | SSD1306 (Adafruit) | KCT8103L | если панель SSD1306 |
| `generic_sx1262` | любой ESP32-S3 + SX1262 | нет (serial-only) | нет | минимальный дебаг |

Все платы используют одинаковый radio-профиль (см.
[параметры радио](#параметры-радио-и-каналы)).

## Как добавить свою плату

1. Скопируйте секцию env в `platformio.ini` и поменяйте:
   - `board = <ваша плата>` (или `esp32-s3-devkitc-1`);
   - пины SX1262: `-DP_LORA_NSS/-DP_LORA_DIO_1/-DP_LORA_RESET/-DP_LORA_BUSY`
     и шину `-DP_LORA_SCLK/-DP_LORA_MISO/-DP_LORA_MOSI`;
   - при наличии OLED: `-DHAS_OLED=1`, `-DPIN_BOARD_SDA/-DPIN_BOARD_SCL`,
     `-DPIN_OLED_RESET`, при наличии FEM — `-DP_LORA_PA_POWER`,
     `-DP_LORA_KCT8103L_PA_CSD`, `-DP_LORA_KCT8103L_PA_CTX`;
   - при рейле питания `-DPIN_VEXT_EN` + `-DPIN_VEXT_EN_ACTIVE`
     (активный уровень HIGH/LOW), иначе просто не указывайте.
2. `src/board_config.h` сам состыкует флаги с кодом; для экзотики можно
   заложить свои `HAS_OLED/HAS_FEM/OLED_DRIVER_SH1106`.

Флаги и их дефолты (дефолт = Heltec V4.3), полная карта — в
`src/board_config.h`.

## Русский шрифт на OLED

Adafruit GFX умеет только ASCII; кириллица поставляется через хук виртуального
`Adafruit_GFX::write(uint8_t)`: входящий UTF-8 стримингово превращается в
CP866, и каждый глиф рисуется по таблице 6x8 битмапов из
`src/cyrillic_glyphs.h` (шаг/высота совместимы с classic-шрифтом GFX).
Перенос строк и `\n` обрабатываются автоматически; `print("Привет")` работает
как обычно.

Работает одинаково для обоих драйверов:
- `sysoled.h` (SH1106) — свой драйвер;
- `RusSSD1306` (SSD1306, Adafruit) — тот же хук.

Перегенерация таблицы:

```bash
pip install pillow fonttools     # один раз
python3 scripts/gen_cyrillic_font.py
```

Скрипт рендерит DejaVuSansMono (~10px), бьёт на CP866-индексы и кладёт
`src/cyrillic_glyphs.h` + `src/_preview.png` (визуальный чек без прошивки).
Проблемные глифы (например «Ш») можно поправить вручную в `cyrillic.h` —
там есть выделенное место с оверрайдами; тривиально также добавить свои
ASCII-умы и символы после `0xF0`.

## Параметры радио и каналы

Все параметры — в `platformio.ini` (`[radio_params]`) и в `main.cpp`:

| параметр | значение |
|----------|----------|
| частота | 868.731018 МГц (LoRa EU) |
| bandwidth | 62.5 кГц |
| spreading factor | 8 |
| coding rate | 7 |
| sync word | 0x12 |
| TX power | 10 dBm |
| preamble | 16 |

Каналы:

- `#public` — фиксированный PSK: `sha256` от base64-ключа, hash `0x11`;
- `#connections` — автоключ из имени канала, hash `0x1F`.

Шифрование: AES-128-ECB по первым 16 байтам секрета + HMAC-SHA256
(payload → первые 2 байта) — стандартная схема MeshCore. `txt` содержит
`<имя устройства>: <текст>`.

Личные сообщения (TXT_MSG): `ownShortHash` определяется автоматически как
первый байт Ed25519 pubkey (вычисляется из `DEVICE_NAME` через
`ed25519_create_keypair`); при желании можно переопределить через
`-DBOT_ID_HASH=0xNN`. Бот отвечает на личные пакеты, адресованные ему
(`dest_hash == ownShortHash`): первый ответ — по обратному маршруту
(DIRECT), повтор через 250 мс — флудом теми же байтами (для
дедупликации). Шифрование ответа — X25519 shared secret
(`ed25519_key_exchange`), AES-128-ECB + HMAC-SHA256; pubkey отправителя
кэшируется из ADVERT-пакетов (с Ed25519-верификацией подписи).

Адвертинг: direct-реклам каждые 5 минут, flood — каждые 30; boot-адверт
уходит через ~6 с после старта. Подпись — ed25519 (ключ зернится из
`SHA256(DEVICE_NAME)`). `/ping` отвечает на `#connections`: если пакет пришёл
напрямую — `hops:direct`, иначе `hops:N, route:aa → bb`.

## MQTT / Home Assistant

Опциональная интеграция с Home Assistant через MQTT (Auto-Discovery).
Компилируется только при `-DMQTT_ENABLED`; без этого флага код WiFi/MQTT
полностью исключается из сборки.

### Включение

1. В `platformio.ini` раскомментируйте env `heltec_v4_3_mqtt` и заполните:
   ```
   -DWIFI_SSID=\"YourSSID\"
   -DWIFI_PASS=\"YourPassword\"
   -DMQTT_BROKER=\"192.168.1.100\"
   -DMQTT_PORT=1883
   ```

2. В Home Assistant установите интеграцию [MQTT](https://www.home-assistant.io/integrations/mqtt/)
   (она уже может быть — если HA подключён к Mosquitto/EMQX/etc).

3. Прошейте бота. После подключения к WiFi и MQTT бот автоматически
   создаст сущности в HA через MQTT Discovery.

### Сущности в HA

| Тип | Имя | Описание |
|-----|-----|----------|
| **Sensor** | `<имя> Message` | Последнее сообщение: sender, text, channel, RSSI, SNR, hops, route |
| **Sensor** | `<имя> LastMsg` | Текст последнего сообщения **выбранного** канала (удобно для триггеров) |
| **Sensor** | `<имя> Status` | Системный статус: uptime, packets, duplicates, channel, private |
| **Text** | `<имя> Send` | Текстовое поле → LoRa TX (флуд в выбранный канал) |
| **Select** | `<имя> Channel` | Выбор канала TX: `#public` / `#connections` / приватный канал |
| **Text** | `<имя> Priv Chan Name` | Имя приватного канала для прослушки, напр. `#garage` |
| **Text** | `<имя> Priv Chan Key` | PSK приватного канала (base64, 16 байт; пусто = автоключ из имени) |
| **Switch** | `<имя> Listening` | Вкл/выкл приёмника (radio.startReceive / radio.standby) |

### MQTT Topics

| Topic | Direction | Описание |
|-------|-----------|----------|
| `meshcore/bot/<name>/state` | → HA | JSON: последнее сообщение |
| `meshcore/bot/<name>/lastmsg` | → HA | Текст сообщения ВЫБРАННОГО канала (для триггеров) |
| `meshcore/bot/<name>/status` | → HA | JSON: статус (uptime, packets, channel, private, ...) |
| `meshcore/bot/<name>/lstate` | → HA | `"ON"` / `"OFF"` (listening state) |
| `meshcore/bot/<name>/cmd/send` | ← HA | Текст → LoRa TX |
| `meshcore/bot/<name>/cmd/channel` | ← HA | `"#public"` / `"#connections"` / приватный канал |
| `meshcore/bot/<name>/cmd/listening` | ← HA | `"ON"` / `"OFF"` |
| `meshcore/bot/<name>/cmd/private/name` | ← HA | Имя приватного канала (напр. `#garage`) |
| `meshcore/bot/<name>/cmd/private/key` | ← HA | PSK base64 (16 байт); пусто = автоключ |

### Приватный канал

Задаётся из HA двумя полями: **имя** (например `#garage`) и **ключ**. Если
ключ пустой — используется автоключ `SHA256(имя)[0:16]` (как у
`#connections`); если задан — это PSK в base64 (16 байт, как у `#public`).
Канал добавляется третьим в таблицу, и бот слушает его вместе с
`#public`/`#connections`. Имя + ключ сохраняются в NVS и восстанавливаются
после перезагрузки. После смены имени/ключа discovery перепубликуется.

### Статус на OLED

При включённом MQTT на экране рядом с «Listening...» показывается строка
`WiFi:+ MQTT:+` (`+` = подключено, `-` = нет).

### Размер прошивки

| Вариант | RAM | Flash |
|---------|-----|-------|
| Без MQTT | 7.2% (23 КБ) | 12.2% (408 КБ) |
| С MQTT | 15.1% (49 КБ) | 25.2% (843 КБ) |

## Драйвер OLED (SH1106 vs SSD1306)

Heltec V4 поставляется с двумя вариантами панелей. На «новых партиях» стоит
SH1106-панель, и SSD1306-инит даёт горизонтальные полосы без текста,
поэтому по умолчанию включён свой SH1106-драйвер (`OLED_DRIVER_SH1106=1`).
Если на вашей плате нормальный SSD1306 — соберите env `heltec_v4_3_ssd1306`
или поставьте `-DOLED_DRIVER_SH1106=0`.