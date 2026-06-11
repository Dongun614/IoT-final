/*
 * TTGO LoRa32 — 서버 보드 펌웨어 (수신 전용)
 *
 * 역할: LoRa RF 수신 → UART JSON 출력 (서버 PC로 전달)
 *
 * 출력 형식:
 *   {"type":"rx","rssi":-72,"snr":8.5,"len":11,"data":"<hex>"}
 *
 * server_rx.py가 이 JSON을 파싱하여 robot_payload_t로 디코딩.
 * 송신 코드 없음 (채널 오염 방지).
 */

#include <SPI.h>
#include <LoRa.h>

#define LORA_SS   18
#define LORA_RST  14
#define LORA_DIO0 26

#define LORA_FREQ  915E6
#define SYNC_WORD  0xAB

static volatile bool packetReady = false;
static uint8_t  rxBuf[255];
static int      rxLen   = 0;
static int      rxRssi  = 0;
static float    rxSnr   = 0.0f;

/* 인터럽트 컨텍스트에서 호출 — 빠르게 복사만 수행. */
static void onReceive(int packetSize) {
    if (packetSize == 0 || packetSize > (int)sizeof(rxBuf)) return;
    rxLen  = 0;
    rxRssi = LoRa.packetRssi();
    rxSnr  = LoRa.packetSnr();
    while (LoRa.available() && rxLen < (int)sizeof(rxBuf))
        rxBuf[rxLen++] = (uint8_t)LoRa.read();
    packetReady = true;
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

    LoRa.setSyncWord(SYNC_WORD);
    LoRa.setSpreadingFactor(9);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    /* 송신 설정 없음 */

    LoRa.onReceive(onReceive);
    LoRa.receive();  /* 수신 모드 진입 */

    Serial.println("{\"type\":\"ready\",\"role\":\"server\"}");
}

void loop() {
    if (!packetReady) return;

    /* 로컬 복사 후 플래그 해제 */
    int     len  = rxLen;
    int     rssi = rxRssi;
    float   snr  = rxSnr;
    uint8_t buf[255];
    memcpy(buf, rxBuf, (size_t)len);
    packetReady = false;

    /* JSON 출력 */
    Serial.print("{\"type\":\"rx\",\"rssi\":");
    Serial.print(rssi);
    Serial.print(",\"snr\":");
    Serial.print(snr, 1);
    Serial.print(",\"len\":");
    Serial.print(len);
    Serial.print(",\"data\":\"");
    for (int i = 0; i < len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", buf[i]);
        Serial.print(hex);
    }
    Serial.println("\"}");

    /* 수신 모드 재진입 */
    LoRa.receive();
}
