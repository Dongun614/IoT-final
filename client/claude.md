# Wi-Fi / LoRa 하이브리드 프로토콜 — 프로젝트 컨텍스트

## 프로젝트 개요

다중 로봇 환경에서 Wi-Fi와 LoRa를 RSSI 기반 FSM으로 자동 전환하는 하이브리드 통신 프로토콜 구현.
핵심 목표: 전환 순간 데이터 손실 최소화, Ping-pong 방지, 관제 투명성.

**개발 환경**: macOS (개발) → Raspberry Pi (실행), SSH + VSCode Remote 방식  
**현재 목표**: 로봇 RPi 코드 구현 — FSM + MQTT + LoRa UART

---

## 시스템 구성 (3노드)

```
[로봇 RPi] ──Wi-Fi──▶ [서버 PC : Mosquitto] ──▶ [관제 브라우저]
[로봇 RPi] ──UART──▶ [TTGO 로봇] ──LoRa RF──▶ [TTGO 서버] ──UART──▶ [서버 PC C 브리지] ──▶ Mosquitto
```

---

## 하드웨어

### 로봇 RPi
- Raspberry Pi (OS: Raspberry Pi OS Bookworm, Python 3.11 기본 탑재)
- Wi-Fi 내장 (wlan0)
- TTGO LoRa32-OLED 보드를 USB 시리얼로 연결 (`/dev/lora_robot` — udev rule로 고정)

### TTGO LoRa32-OLED (로봇용, 송신 브리지)
- ESP32 기반
- LoRa 모듈: SX1276 계열
- Arduino IDE로 펌웨어 개발
- LoRa 라이브러리: sandeepmistry/arduino-LoRa
- setPins(18, 14, 26), begin(915E6), setTxPower(20)
- RPi와 USB CDC 시리얼로 통신 (baudrate: 115200)
- 역할: RPi UART로 받은 바이너리 패킷 → LoRa RF 송신 (CSMA/CA 포함)
- 송신 전 CAD(Channel Activity Detection) 수행 → 채널 사용 중이면 랜덤 백오프

### TTGO LoRa32-OLED (서버용, 수신 전용)
- 동일 보드, 다른 펌웨어
- 수신 전용 — 송신 코드 없음
- LoRa 수신 → UART로 서버 PC C 브리지에 전달
- RSSI, SNR 포함 전달

### 서버 PC
- Mosquitto MQTT 브로커 실행 (포트 1883)
- TTGO 서버 USB 연결 (`/dev/lora_server`)
- C 브리지 프로세스: UART 수신 → robot_id 파싱 → MQTT publish
- Python 프로세스: MQTT subscribe → WebSocket → 관제 브라우저

---

## 언어 및 기술 스택

| 구성 요소 | 언어 | 주요 라이브러리 |
|---|---|---|
| TTGO 펌웨어 (로봇/서버) | Arduino C++ | arduino-LoRa (sandeepmistry) |
| 로봇 RPi | C | libmosquitto, termios.h |
| 서버 C 브리지 | C | libmosquitto, termios.h |
| 서버 WebSocket | Python 3.11+ | paho-mqtt, websockets |
| 관제 대시보드 | HTML / JS | WebSocket API |

---

## 로봇 RPi 코드 구조 (현재 구현 대상)

### 파일 구조 (목표)
```
robot/
├── main.c          # 진입점, 메인 루프
├── fsm.c / fsm.h   # FSM 상태 머신
├── mqtt.c / mqtt.h # libmosquitto 래퍼
├── uart.c / uart.h # termios UART 래퍼 (TTGO 시리얼)
├── rssi.c / rssi.h # Wi-Fi RSSI 측정
├── payload.h       # 공통 페이로드 구조체
└── Makefile
```

### FSM 상태 정의
```c
typedef enum {
    STATE_NORMAL  = 0,  // Wi-Fi 송수신 / LoRa Sleep
    STATE_WARNING = 1,  // Wi-Fi 유지 + LoRa Wake (대기)
    STATE_LORA    = 2   // LoRa 전용 / Wi-Fi 비콘 스캔
} fsm_state_t;
```

### FSM 전환 조건
- NORMAL → WARNING: RSSI < T1 (초기값 −65 dBm)
- WARNING → LORA: RSSI < T2 (초기값 −80 dBm) OR Wi-Fi 완전 끊김
- WARNING → NORMAL: 안정화 조건 충족 (N초 동안 RSSI 샘플 M개 중 K개 이상 T1 이상)
- LORA → WARNING: Wi-Fi 비콘 감지 + Handshake 완료

### WARNING 구간 동작 (Ping-pong 방지 핵심)
- Wi-Fi 살아있으면 → Wi-Fi로만 전송, LoRa는 켜져 있지만 전송 안 함
- Wi-Fi 끊기면 → LoRa로 전송, Wi-Fi 재연결 시도 계속
- Wi-Fi 재연결 성공해도 → 안정화 조건 충족 전까지 Wi-Fi 전송 대기
- 안정화 조건 초기값: 10초 / 20샘플 / 15개 이상 T1 이상 (실측 후 조정)

### 버퍼링
- 전환 순간 전송 못 한 패킷 → 메모리 링버퍼 보관
- 새 채널 확정 후 seq 순서대로 flush

---

## 페이로드 구조체

```c
// payload.h
#pragma pack(1)
typedef struct {
    uint8_t  robot_id;  // 로봇 식별자 (1부터 시작)
    uint16_t seq;       // 시퀀스 번호 (dedup / 손실 감지)
    uint32_t ts;        // Unix timestamp (로봇 발신 시각)
    uint8_t  state;     // FSM 상태 (0=NORMAL, 1=WARNING, 2=LORA)
    int8_t   rssi;      // Wi-Fi RSSI (dBm)
    int16_t  temp;      // 온도 ×100 (예: 2350 = 23.50°C)
                        // RPi CPU 온도: /sys/class/thermal/thermal_zone0/temp
} robot_payload_t;      // 총 11 byte
#pragma pack()
```

- LoRa 최대 255 byte — 11 byte 헤더로 여유 충분
- temp는 `/sys/class/thermal/thermal_zone0/temp` 읽어서 사용 (별도 센서 불필요)
- Wi-Fi 경로: JSON도 가능 (프로토타입 단계), LoRa 경로: 반드시 바이너리 구조체

---

## MQTT 토픽 구조

| 토픽 | 방향 | 주체 |
|---|---|---|
| `robot/{id}/status` | 업링크 | RPi (Wi-Fi) 또는 서버 C 브리지 (LoRa) |
| `robot/+/status` | subscribe | 서버 Python |
| `robot/{id}/cmd` | 다운링크 | 서버 Python → RPi |

### CMD 페이로드 (JSON)
```json
{
  "tx_power": 15,
  "t1_rssi": -65,
  "t2_rssi": -80,
  "stable_sec": 10,
  "sample_n": 20
}
```
- RPi가 수신 후 FSM 파라미터 실시간 업데이트
- tx_power는 UART로 TTGO에 전달 → LoRa.setTxPower() 호출

---

## UART (TTGO 시리얼) 프로토콜

### RPi → TTGO (송신 요청)
```
[1 byte: cmd] [n byte: data] [1 byte: '\n']

cmd 종류:
  0x01 = 패킷 전송 요청 (data = robot_payload_t 바이너리)
  0x02 = TxPower 변경 (data = 1 byte, 값: 2~20)
  0x03 = Sleep 요청
  0x04 = Wake 요청
```

### TTGO → RPi (수신 알림 / 상태)
```json
{"type":"rx","rssi":-72,"snr":8.5,"data":"<hex>"}
{"type":"tx_done"}
{"type":"cad","busy":false}
```

---

## RSSI 측정 방법 (RPi)

```bash
# iw 명령어로 RSSI 읽기
iw dev wlan0 link | grep signal
# 출력 예: signal: -72 dBm
```

C에서는 `popen("iw dev wlan0 link", "r")`으로 파싱하거나,
`/proc/net/wireless` 파일을 직접 읽는 방식 사용.

---

## udev Rule (시리얼 포트 고정)

```bash
# /etc/udev/rules.d/99-lora.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{serial}=="<로봇TTGO시리얼>", SYMLINK+="lora_robot"
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{serial}=="<서버TTGO시리얼>", SYMLINK+="lora_server"

# 시리얼 번호 확인 방법
udevadm info /dev/ttyUSB0 | grep SERIAL
```

---

## Mosquitto 설정 (서버 PC)

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
# 포트: 1883 (평문), 익명 접속 허용 (개발 단계)
```

---

## 우려사항 및 해결 방향 요약

| # | 문제 | 해결 |
|---|---|---|
| W1 | 시리얼 포트 번호 불안정 | udev rule로 고정 |
| W2 | 전환 순간 데이터 유실 | 메모리 버퍼 + WARNING 이중 채널 |
| W3 | Ping-pong 현상 | WARNING 구간 + 안정화 조건 |
| W4 | C↔Python 타이밍 | localhost 통신으로 무시 수준 |
| W5 | LoRa robot_id 식별 불가 | 페이로드 첫 바이트에 항상 포함 |
| W6 | 다중 로봇 LoRa 충돌 | CSMA/CA + CAD + 랜덤 백오프 |
| W7 | 서버 TTGO 채널 오염 | 서버 TTGO 펌웨어에 송신 코드 없음 |
| W8 | WARNING 구간 중복 수신 | (robot_id, seq) dedup, 200ms 윈도우 |

---

## 실측 필요 항목 (구현 후 튜닝)

- T1, T2 RSSI 임계값 (환경마다 다름)
- TTGO 웨이크업 시간 (Sleep → 첫 전송)
- 안정화 조건 수치 (시간, 샘플 수)
- CSMA/CA 백오프 범위

모든 파라미터는 `robot/{id}/cmd` 토픽으로 런타임 수정 가능하도록 구현할 것.

---

## 개발 순서 (권장)

1. `payload.h` 구조체 정의
2. `rssi.c` — iw 명령어 파싱으로 RSSI 읽기
3. `fsm.c` — 상태 전환 로직 (Mock RSSI로 먼저 테스트)
4. `uart.c` — termios UART 송수신 (/dev/lora_robot)
5. `mqtt.c` — libmosquitto publish/subscribe 래퍼
6. `main.c` — 통합 루프
7. TTGO 펌웨어 (Arduino C++) — 송신 브리지
8. 전체 파이프라인 통합 테스트