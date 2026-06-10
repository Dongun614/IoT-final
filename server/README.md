# 서버 구현 — Wi-Fi/LoRa 하이브리드 프로토콜

## 환경
- **OS**: macOS (Intel)
- **Mosquitto**: 2.1.2 (Homebrew)
- **Python**: 3.x
- **위치**: `~/Desktop/IoT final/server/`

---

## 파일 구조

```
server/
├── lora_bridge.c   # TTGO UART → MQTT 브리지 (C)
├── Makefile        # C 브리지 빌드
├── server.py       # MQTT → WebSocket 브리지 (Python)
├── index.html      # 관제 대시보드 (브라우저)
└── README.md
```

---

## 최초 설치 (1회만)

```bash
# Mosquitto
brew install mosquitto
brew services start mosquitto

# Python 패키지
pip3 install paho-mqtt websockets

# C 브리지 빌드
cd ~/Desktop/IoT\ final/server
make
```

---

## 실행 방법

### 1. Mosquitto (재부팅 후 자동 실행됨, 평소엔 생략)
```bash
brew services start mosquitto
```

### 2. Python 서버 (터미널 1)
```bash
cd ~/Desktop/IoT\ final/server
python3 server.py
```

### 3. C 브리지 (터미널 2 — TTGO 연결 후)
TTGO를 USB에 꽂고 포트 확인:
```bash
ls /dev/cu.*
```
나온 포트 이름으로 실행:
```bash
cd ~/Desktop/IoT\ final/server
./lora_bridge /dev/cu.usbserial-XXXX
```

### 4. 대시보드
```bash
open ~/Desktop/IoT\ final/server/index.html
```

---

## 역할 정리

| 구성요소 | 역할 |
|---|---|
| Mosquitto | MQTT 브로커. Wi-Fi 경로 패킷은 여기로 바로 들어옴 |
| lora_bridge.c | 서버 TTGO UART 수신 → JSON 변환 → MQTT publish |
| server.py | MQTT 구독 → dedup → WebSocket 브로드캐스트, 커맨드 역방향 중계 |
| index.html | 관제 대시보드. 브라우저에서 직접 열면 됨 |

---

## UART 프레임 포맷 (lora_bridge 기준)

로봇 팀이 TTGO 아두이노에서 UART로 내려줄 때 아래 포맷을 맞춰야 함.

```
[0xAA][robot_id:1B][seq:2B][ts:4B][state:1B][wifi_rssi:1B][temp:2B][lora_rssi:1B][lora_snr:1B]
 magic  ←──────────────── 11 byte 페이로드 ────────────────→  ←─ LoRa 신호 품질 ─→
총 14 byte
```

| 필드 | 타입 | 크기 | 설명 |
|---|---|---|---|
| magic | - | 1B | 항상 0xAA (프레임 시작 표시) |
| robot_id | uint8 | 1B | 로봇 식별자 |
| seq | uint16 | 2B | 패킷 순서 번호 |
| ts | uint32 | 4B | Unix timestamp |
| state | uint8 | 1B | 0=NORMAL, 1=WARNING, 2=LORA_ONLY |
| wifi_rssi | int8 | 1B | Wi-Fi 신호 세기 (dBm) |
| temp | int16 | 2B | 온도 × 100 (예: 2350 = 23.50°C) |
| lora_rssi | int8 | 1B | LoRa 수신 신호 세기 (dBm) |
| lora_snr | int8 | 1B | LoRa SNR (dB) |

---

## MQTT 토픽

| 토픽 | 방향 | 내용 |
|---|---|---|
| `robot/{id}/status` | 로봇→서버 | 상태 데이터 (JSON) |
| `robot/{id}/cmd` | 서버→로봇 | 파라미터 커맨드 (JSON) |

커맨드 JSON 예시:
```json
{"tx_power": 15, "t1_rssi": -65, "t2_rssi": -80, "stable_sec": 10}
```

---

## 포트

| 서비스 | 포트 |
|---|---|
| Mosquitto | 1883 |
| WebSocket | 8765 |
