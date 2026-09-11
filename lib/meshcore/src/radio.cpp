#include "config.h"
#include "globals.h"
#include "radio.h"

void rearmRadioAGC() {
    radio.sleep();
    radio.startReceive();
    lastReArmMs = millis();
}

int txFrame(uint8_t* frame, int f) {
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, HIGH);
    #endif
    int st = radio.transmit(frame, f);
    #if HAS_FEM
    digitalWrite(FEM_TX_PIN, LOW);
    #endif

    if (st == RADIOLIB_ERR_NONE) {
        Serial.println("[TX] OK");
    } else {
        Serial.printf("[TX] FAILED %d\n", st);
    }

    // после TX обязательно вернуться в RX; при ошибке — полный ре-арм AGC
    if (radio.startReceive() != RADIOLIB_ERR_NONE) {
        rearmRadioAGC();
    }
    return st;
}

// SX1262-специфичные настройки включаются build_flags'ами
// (см. platformio.ini / board_config.h), чтобы на других платах
// не применять опции, нужные только Heltec V4. begin/beginFSK их сбрасывают.
static void applyBoardRadioOptions() {
    #ifdef SX126X_DIO2_AS_RF_SWITCH
    radio.setDio2AsRfSwitch(true);
    #endif
    #ifdef SX126X_RX_BOOSTED_GAIN
    radio.setRxBoostedGainMode(true);
    #endif
    #ifdef SX126X_CURRENT_LIMIT
    radio.setCurrentLimit(SX126X_CURRENT_LIMIT);
    #endif
    #ifdef SX126X_REGISTER_PATCH
    // патч регистра 0x8B5 для улучшенного приёма на Heltec v4
    uint8_t r_data = 0;
    radio.readRegister(0x8B5, &r_data, 1);
    r_data |= 0x01;
    radio.writeRegister(0x8B5, &r_data, 1);
    #endif
}

bool initLoRa() {
    Serial.println("Init LoRa...");
    
    // Вызов begin() БЕЗ параметра TCXO.
    // RadioLib возьмёт значение из макроса SX126X_DIO3_TCXO_VOLTAGE,
    // который мы определим в platformio.ini.
    int state = radio.begin(
        LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
        LORA_SYNC_WORD, LORA_TX_POWER, LORA_PREAMBLE, 1.8
    );
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("LoRa OK (TCXO from macro)");
        radio.setCRC(true);
        applyBoardRadioOptions();
        return true;
    }
    
    Serial.printf("LoRa FAILED: %d\n", state);
    return false;
}

void radioSetParams(float freq, float bw, int sf, int cr) {
    radio.standby();
    radio.setFrequency(freq);
    radio.setBandwidth(bw);
    radio.setSpreadingFactor(sf);
    radio.setCodingRate(cr);
    lastReArmMs = 0;           // разрешить AGC rearm сразу
    radio.startReceive();
    isListening = true;
}

static bool radioFsk = false;

void radioSetNormalConfig() {
    if (radioFsk) {
        radioFsk = false;
        initLoRa();
        lastReArmMs = 0;
        radio.startReceive();
        isListening = true;
        return;
    }
    radioSetParams(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR);
}

void radioSetFastConfig() {
    #if OTA_FAST_FSK
    int st = radio.beginFSK(OTA_FAST_FREQ, OTA_FSK_BR, OTA_FSK_DEV, OTA_FSK_RXBW,
                            LORA_TX_POWER, OTA_FSK_PREAMBLE, 1.8);
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[RADIO] beginFSK failed %d\n", st);
    radioFsk = true;
    applyBoardRadioOptions();
    radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    uint8_t sw[] = { 0xBE, 0xEF, 0x07, 0xA5 };
    radio.setSyncWord(sw, sizeof(sw));
    lastReArmMs = 0;
    radio.startReceive();
    isListening = true;
    #else
    radioSetParams(OTA_FAST_FREQ, OTA_FAST_BW, OTA_FAST_SF, OTA_FAST_CR);
    #endif
}
