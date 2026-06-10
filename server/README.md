# 서버 구현 — Wi-Fi/LoRa 하이브리드 프로토콜

## 디렉토리 구조

```
server/
├── bridge/
│   ├── lora_bridge.c     # TTGO UART → MQTT 브리지 (C)
│   └── Makefile
├── python/
│   └── server.py         # MQTT → WebSocket 브리지 (Python)
├── dashboard/
│   └── index.html        # 관제 대시보드 (브라우저)
├── 99-lora.rules         # udev 시리얼 포트 고정
└── README.md
```

---

## 1단계 — 의존성 설치

```bash
# Mosquitto 브로커 + C 라이브러리
sudo apt install mosquitto mosquitto-clients libmosquitto-dev

# Python 패키지
pip3 install paho-mqtt websockets

# Mosquitto 자동 시작
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

---

## 2단계 — 시리얼 포트 고정 (udev)

TTGO를 USB에 꽂고 시리얼 번호 확인:

```bash
udevadm info -a -n /dev/ttyUSB0 | grep serial
```

`99-lora.rules` 안의 `REPLACE_SERVER_SERIAL`을 실제 값으로 교체한 후:

```bash
sudo cp 99-lora.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

이후 `/dev/lora_server`로 고정 접근 가능.

---

## 3단계 — C 브리지 빌드

```bash
cd bridge
make
```

---

## 4단계 — 실행 (터미널 3개)

**터미널 1 — Mosquitto (이미 서비스로 실행 중이면 생략)**
```bash
mosquitto -v
```

**터미널 2 — C 브리지**
```bash
cd bridge
./lora_bridge /dev/lora_server
# udev 미설정 시: ./lora_bridge /dev/ttyUSB0
```

**터미널 3 — Python 서버**
```bash
cd python
python3 server.py
```

**대시보드**
브라우저에서 `dashboard/index.html` 파일 열기
(또는 `python3 -m http.server 8080` 으로 서빙 후 `http://localhost:8080`)

---

## 페이로드 포맷 (TTGO UART 프레임)

TTGO 아두이노가 UART로 내려줄 때의 프레임 구조:

```
[0xAA][robot_id:1B][seq:2B][ts:4B][state:1B][wifi_rssi:1B][temp:2B][lora_rssi:1B][lora_snr:1B]
 magic  ←────────────── 11 byte 페이로드 ──────────────→  ←── LoRa 신호 품질 ──→
총 14 byte
```

**state 값**: 0=NORMAL, 1=WARNING, 2=LORA_ONLY

**temp 인코딩**: °C × 100 정수 (예: 2350 = 23.50°C)

---

## MQTT 토픽

| 토픽 | 방향 | 내용 |
|------|------|------|
| `robot/{id}/status` | 로봇→서버 | 상태 데이터 (JSON) |
| `robot/{id}/cmd` | 서버→로봇 | 파라미터 커맨드 (JSON) |

커맨드 JSON 예시:
```json
{"tx_power": 15, "t1_rssi": -65, "t2_rssi": -80, "stable_sec": 10}
```

---

## 포트 정리

| 서비스 | 포트 | 프로토콜 |
|--------|------|----------|
| Mosquitto | 1883 | MQTT/TCP |
| WebSocket | 8765 | WS |
| 대시보드 (옵션) | 8080 | HTTP |

관제 PC는 서버 IP의 8765 포트로 WebSocket 연결하면 됨.
(같은 노트북이면 `localhost:8765`)
