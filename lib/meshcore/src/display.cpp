#include "config.h"
#include "globals.h"
#include "crypto.h"
#include "display.h"

void drawIdleStatus() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    // часы из системного времени (обновляются каждые 500 мс вместе с экраном)
    time_t now = time(NULL) + (time_t)TZ_OFFSET_HOURS * 3600;
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S  %d.%m", &tm_now);
    unsigned long up = millis() / 1000;
    #ifdef SENSOR_NODE
    display.println(DEVICE_NAME);
    display.println(tbuf);
    display.printf("Up: %luh%02lum\n", up / 3600, (up % 3600) / 60);
    if (sensorLastSent.length() > 0) {
        display.printf("TX: %s\n", sensorLastSent.substring(0, 17).c_str());
    }
    #else
    #ifdef MQTT_ENABLED
    // WiFi/MQTT статус: 1 строка, "+" = подключено, "-" = нет
    const char* w = wifiConnected ? "+" : "-";
    const char* m = mqttConnected ? "+" : "-";
    display.printf("WiFi:%s MQTT:%s\n", w, m);
    if (mqttTxChannel >= 0 && mqttTxChannel < numChannels) {
        display.printf("TX: %s\n", channels[mqttTxChannel].name);
    }
    #endif
    display.println(tbuf);
    // uptime + температура CPU (встроенный датчик ESP32-S3)
    int t = (int)cpuTempC();
    display.printf("Up:%luh%02lum T:%dC\n", up / 3600, (up % 3600) / 60, t);
    display.printf("Pkts: %d\n", packetCount);
    if (lastMessage.length() > 0) {
        display.printf("Last: %s\n", lastMessage.substring(0, 20).c_str());
    }
    #endif
    display.setCursor(0, 56);
    display.print("v" FW_VERSION);
    display.display();
}
