#include "config.h"
#include "globals.h"
#include "mesh.h"
#include "ota.h"
#include "appconfig.h"
#include "display.h"
#include "button.h"
#include "companion.h"
#include "sensor_tasks.h"

// ===== Периодические задачи узла =====
// Выделено из главного цикла: сторож прошивки по радио, ожидание ответа на проверку
// связи, откат несохранённых настроек, обслуживание телефонного приложения, гашение
// экрана, частота процессора и расписание heartbeat. Всё это работает по времени и
// ничего не ждёт, поэтому собрано в одном месте отдельно от приёма радио.

#if FEATURE_SENSOR

void sensorTasksTick() {
    screenTick();      // экран гаснет в простое, будит длинное нажатие кнопки
    // Полную частоту держим на всю прошивку, а не только пока открыт быстрый канал.
    // Кадры идут каждые 8 мс, но куда важнее конец сессии: запись во флеш и проверка
    // образа на пониженной частоте — самое подозрительное место, а узел дважды отвергал
    // уже полностью принятый и заведомо исправный образ.
    bool wantFastCpu = otaFastMode || otaActive;
    static bool cpuFast = false;
    if (wantFastCpu != cpuFast) {
        cpuFast = wantFastCpu;
        setCpuFrequencyMhz(cpuFast ? CPU_MHZ_FAST : CPU_MHZ_IDLE);
        Serial.printf("[PWR] частота процессора %d МГц\n", cpuFast ? CPU_MHZ_FAST : CPU_MHZ_IDLE);
    }
    otaSensorTick();   // mesh OTA: сторожевое время — при зависании прерываем сессию
    sensorPingTick();  // не дождались ответа на проверку связи — показать это
    cfgPendingTick();  // правки настроек по радио без "save" откатываются перезагрузкой
    #ifdef COMPANION_NODE
    companionTick();   // кадры от приложения разбираем здесь, а не в колбэке BLE
    #endif
    // Sensor node: button = trigger ("button"), hello = heartbeat раз в N минут
    // Во время OTA mesh-отправки подавляем: радио слушает raw-чанки на быстром канале.
    static unsigned long lastHeartbeat = 0;
    static bool bootHelloSent = false;
    if (!otaActive && cfgReady()) {
        if (!bootHelloSent) {
            bootHelloSent = true;
            sensorSendHello();   // стартовый hello сразу после включения
            // не упреждать первый периодический heartbeat после boot-привета
            lastHeartbeat = millis();
        } else if (sensorHelloDueMs != 0 && millis() >= sensorHelloDueMs) {
            // бот попросил отметиться (кнопка «Опросить» на странице OTA)
            sensorHelloDueMs = 0;
            lastHeartbeat = millis();
            sensorSendHello();
        } else if (millis() - lastHeartbeat >= SENSOR_HEARTBEAT_MS) {
            lastHeartbeat = millis();
            sensorSendHello();
        }
    }
    buttonTick();   // кнопка: счёт нажатий и переключение экрана, без блокировки
}

#endif // FEATURE_SENSOR
