#include "config.h"
#include "globals.h"
#include "mesh.h"
#include "display.h"
#include "button.h"

// ===== Кнопка узла =====
// Выделено из главного цикла. Нажатия считаются без остановки цикла: короткое (меньше
// секунды) идёт в счёт серии — на сенсоре одно «button», два «button2», три запускают
// проверку связи. На компаньоне короткие нажатия не отправляются в сеть вообще (это
// телефон в кармане), работают только тройное и долгое. Долгое нажатие (от двух секунд)
// переключает экран, а промежуток от секунды до двух намеренно не делает ничего, чтобы
// случайное удержание не уходило сообщением в сеть.

#if FEATURE_BUTTON

void buttonTick() {
    // Кнопка: нажатия считаем, не останавливая цикл. Прежний вариант ждал второго и
    // третьего нажатия во вложенных циклах с delay() и задерживал loop() до полутора
    // секунд — на компаньоне это пауза в обслуживании BLE, на сенсоре пропущенные пакеты.
    // Одно нажатие — "button", два — "button2", три — проверка связи с координатором.
    if (!otaActive) {
        static bool btnDown = false;
        static unsigned long btnEdgeMs = 0;   // когда состояние кнопки менялось в последний раз
        static unsigned long btnDownMs = 0;   // когда её нажали
        static int btnPresses = 0;
        bool down = (digitalRead(BUTTON_PIN) == LOW);
        unsigned long now = millis();
        if (down != btnDown && now - btnEdgeMs > 40) {      // 40 мс — подавление дребезга
            btnDown = down;
            btnEdgeMs = now;
            if (down) {
                btnDownMs = now;
            } else {
                unsigned long held = now - btnDownMs;
                if (held >= BTN_WAKE_MS) {
                    screenToggle();             // от двух секунд — переключаем экран
                } else if (held < BTN_TRIGGER_MAX_MS) {
                    btnPresses++;               // короткое нажатие идёт в счёт серии
                }
                // Между секундой и двумя — намеренно ничего: так отсекается случайное
                // удержание кнопки, которое иначе ушло бы сообщением в сеть.
            }
        }
        // Серия закончена: кнопка отпущена и окно ожидания следующего нажатия истекло
        if (btnPresses > 0 && !btnDown && now - btnEdgeMs > SNS_BTN_DBL_WINDOW_MS) {
            int presses = btnPresses;
            btnPresses = 0;
            screenWake();                       // результат должно быть видно
            if (presses >= 3) sensorPingSend();
            // На компаньоне короткие нажатия в сеть не уходят: это телефон в кармане,
            // случайные 1-2 нажатия не должны слать "button"/"button2" в MQTT. Остаются
            // тройное нажатие (проверка связи) и долгое (экран). На сенсоре как было.
            else if (FEATURE_COMPANION == 0) sensorSendMsg(presses == 2 ? SENSOR_MSG_BUTTON2 : SENSOR_MSG_BUTTON);
        }
    }
}

#endif // FEATURE_BUTTON
