/*
 * TTGO LoRa32 — 로봇 보드 펌웨어 (송신 브리지)
 *
 * 역할: Mac/RPi UART → LoRa RF 송신 (CSMA/CA 포함)
 *
 * 라이브러리: LoRa by Sandeep Mistry
 * 보드:       ESP32 Dev Module (또는 TTGO LoRa32)
 * 주파수:     915 MHz
 *
 * UART 프로토콜 (Mac/RPi → TTGO):
 *   [0x01][11 bytes: robot_payload_t]['\n'] → LoRa 송신
 *   [0x02][1 byte: power (2~20)]['\n']     → TxPower 변경
 *   [0x03]['\n']                           → Sleep
 *   [0x04]['\n']                           → Wake
 *
 * UART 응답 (TTGO → Mac/RPi):
 *   {"type":"ready"}
 *   {"type":"tx_done","seq":<n>}
 *   {"type":"tx_fail","attempts":<n>}
 *   {"type":"sleep_ok"}
 *   {"type":"wake_ok"}
 *   {"type":"txpow_ok","power":<n>}
 */

#include <SPI.h>
#include <LoRa.h>

/* TTGO LoRa32 핀 배치 */
#define LORA_SS   18
#define LORA_RST  14
#define LORA_DIO0 26

#define LORA_FREQ   915E6
#define LORA_TX_PWR 20
#define SYNC_WORD   0xAB   /* 같은 네트워크 보드끼리만 통신 */

#define PAYLOAD_SIZE 11    /* robot_payload_t */

#define CMD_TX     0x01
#define CMD_TXPOW  0x02
#define CMD_SLEEP  0x03
#define CMD_WAKE   0x04

/* CSMA/CA 파라미터 */
#define CSMA_MAX_ATTEMPTS  5
#define CSMA_BACKOFF_MIN  20   /* ms */
#define CSMA_BACKOFF_MAX  80   /* ms */

/* 송신 전 채널 활동 감지.
   LoRa를 짧게 수신 모드로 전환해 패킷을 수신 중인지 확인.
   SX1276 패킷 헤더 감지 시간(SF7 기준 ~12ms)을 커버하기 위해 15ms 대기. */
static bool channelClear() {
    LoRa.receive();
    delay(15);
    int pkt = LoRa.parsePacket();
    LoRa.idle();
    return (pkt == 0);
}

/* CSMA/CA 방식으로 LoRa 패킷 송신. */
static bool sendWithCSMA(const uint8_t *data, int len, int *out_attempts) {
    for (int attempt = 1; attempt <= CSMA_MAX_ATTEMPTS; attempt++) {
        *out_attempts = attempt;
        if (!channelClear()) {
            /* 채널 사용 중 → 지수 백오프 */
            long backoff = random(CSMA_BACKOFF_MIN, CSMA_BACKOFF_MAX) * attempt;
            delay(backoff);
            continue;
        }
        if (LoRa.beginPacket()) {
            LoRa.write(data, len);
            if (LoRa.endPacket()) {
                return true;
            }
        }
        delay(random(CSMA_BACKOFF_MIN, CSMA_BACKOFF_MAX));
    }
    return false;
}

/* 시리얼에서 정확히 n바이트 읽기 (타임아웃: ms). */
static int readExact(uint8_t *buf, int n, unsigned long timeout_ms) {
    unsigned long t0 = millis();
    int got = 0;
    while (got < n) {
        if ((millis() - t0) > timeout_ms) return -1;  /* timeout */
        if (Serial.available()) {
            buf[got++] = Serial.read();
        }
    }
    return got;
}

/* '\n' 까지 버림 (프레임 경계 동기화). */
static void drainToNewline(unsigned long timeout_ms) {
    unsigned long t0 = millis();
    while ((millis() - t0) < timeout_ms) {
        if (Serial.available() && Serial.read() == '\n') return;
    }
}

/* robot_payload_t 첫 2바이트에서 seq 추출 (little-endian: bytes[1..2]). */
static uint16_t extractSeq(const uint8_t *payload) {
    return (uint16_t)(payload[1] | ((uint16_t)payload[2] << 8));
}

void setup() {
    Serial.begin(115200);
    while (!Serial);

    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

    int retry = 0;
    while (!LoRa.begin(LORA_FREQ)) {
        if (++retry > 10) {
            Serial.println("{\"type\":\"error\",\"msg\":\"LoRa init failed\"}");
            while (true) delay(1000);
        }
        delay(500);
    }

    LoRa.setTxPower(LORA_TX_PWR);
    LoRa.setSyncWord(SYNC_WORD);
    LoRa.setSpreadingFactor(9);     /* SF9: 범위/속도 균형 */
    LoRa.setSignalBandwidth(125E3); /* 125 kHz */
    LoRa.setCodingRate4(5);         /* 4/5 */
    LoRa.idle();

    Serial.println("{\"type\":\"ready\"}");
}

void loop() {
    if (!Serial.available()) return;

    uint8_t cmd = Serial.read();

    if (cmd == CMD_TX) {
        uint8_t payload[PAYLOAD_SIZE];
        if (readExact(payload, PAYLOAD_SIZE, 200) != PAYLOAD_SIZE) {
            drainToNewline(100);
            Serial.println("{\"type\":\"tx_fail\",\"reason\":\"short_read\"}");
            return;
        }
        drainToNewline(50);  /* '\n' 소비 */

        uint16_t seq = extractSeq(payload);
        int attempts = 0;
        if (sendWithCSMA(payload, PAYLOAD_SIZE, &attempts)) {
            Serial.print("{\"type\":\"tx_done\",\"seq\":");
            Serial.print(seq);
            Serial.print(",\"attempts\":");
            Serial.print(attempts);
            Serial.println("}");
        } else {
            Serial.print("{\"type\":\"tx_fail\",\"seq\":");
            Serial.print(seq);
            Serial.print(",\"attempts\":");
            Serial.print(attempts);
            Serial.println("}");
        }

    } else if (cmd == CMD_TXPOW) {
        uint8_t power = 0;
        if (readExact(&power, 1, 100) == 1) {
            if (power < 2)  power = 2;
            if (power > 20) power = 20;
            LoRa.setTxPower(power);
        }
        drainToNewline(50);
        Serial.print("{\"type\":\"txpow_ok\",\"power\":");
        Serial.print(power);
        Serial.println("}");

    } else if (cmd == CMD_SLEEP) {
        drainToNewline(50);
        LoRa.sleep();
        Serial.println("{\"type\":\"sleep_ok\"}");

    } else if (cmd == CMD_WAKE) {
        drainToNewline(50);
        LoRa.idle();
        Serial.println("{\"type\":\"wake_ok\"}");

    } else {
        /* 알 수 없는 명령 → 프레임 재동기화 */
        drainToNewline(100);
    }
}
