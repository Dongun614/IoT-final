"""
server.py
MQTT → WebSocket 브릿지 + CMD 역방향 중계

설치: pip install paho-mqtt websockets
실행: python3 server.py
"""

import asyncio
import json
import logging
import time
from collections import defaultdict

import paho.mqtt.client as mqtt
import websockets
from websockets.server import WebSocketServerProtocol

# ─── 설정 ───────────────────────────────────────────────
MQTT_HOST       = "localhost"
MQTT_PORT       = 1883
MQTT_TOPIC_SUB  = "robot/+/status"
MQTT_TOPIC_CMD  = "robot/{id}/cmd"
WS_HOST         = "0.0.0.0"
WS_PORT         = 8765
DEDUP_WINDOW_MS = 200      # 중복 제거 윈도우 (ms)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("server")

# ─── 공유 상태 ──────────────────────────────────────────
connected_ws: set[WebSocketServerProtocol] = set()

# dedup: { (robot_id, seq): last_seen_ms }
dedup_cache: dict[tuple, float] = {}

# 로봇별 마지막 상태 (새 클라이언트 접속 시 즉시 전송용)
last_status: dict[int, dict] = {}

# asyncio 이벤트 루프 참조 (MQTT 콜백에서 사용)
_loop: asyncio.AbstractEventLoop | None = None

# ─── dedup 처리 ─────────────────────────────────────────
def is_duplicate(robot_id: int, seq: int) -> bool:
    now_ms = time.time() * 1000
    key = (robot_id, seq)

    # 만료된 항목 정리
    expired = [k for k, t in dedup_cache.items() if now_ms - t > DEDUP_WINDOW_MS * 10]
    for k in expired:
        del dedup_cache[k]

    if key in dedup_cache:
        if now_ms - dedup_cache[key] < DEDUP_WINDOW_MS:
            return True  # 중복

    dedup_cache[key] = now_ms
    return False

# ─── WebSocket 브로드캐스트 ──────────────────────────────
async def broadcast(message: str):
    if not connected_ws:
        return
    dead = set()
    for ws in connected_ws.copy():
        try:
            await ws.send(message)
        except Exception:
            dead.add(ws)
    connected_ws -= dead

# ─── MQTT 콜백 ──────────────────────────────────────────
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        log.info("Mosquitto 연결 성공")
        client.subscribe(MQTT_TOPIC_SUB)
        log.info(f"구독: {MQTT_TOPIC_SUB}")
    else:
        log.error(f"Mosquitto 연결 실패: rc={rc}")

def on_disconnect(client, userdata, rc):
    if rc != 0:
        log.warning(f"Mosquitto 연결 끊김 (rc={rc}), 재연결 대기 중...")

def on_message(client, userdata, msg):
    global _loop
    try:
        data = json.loads(msg.payload.decode())
    except Exception as e:
        log.warning(f"JSON 파싱 실패: {e}")
        return

    robot_id = data.get("robot_id", -1)
    seq      = data.get("seq", -1)

    # dedup
    if is_duplicate(robot_id, seq):
        log.debug(f"중복 패킷 무시: robot={robot_id} seq={seq}")
        return

    # 마지막 상태 저장
    last_status[robot_id] = data

    state_names = {0: "NORMAL", 1: "WARNING", 2: "LORA_ONLY"}
    log.info(
        f"robot={robot_id} seq={seq} "
        f"state={state_names.get(data.get('state'), '?')} "
        f"wifi_rssi={data.get('wifi_rssi')} "
        f"lora_rssi={data.get('lora_rssi')} "
        f"temp={data.get('temp')}"
    )

    # WebSocket으로 전달
    envelope = json.dumps({"type": "status", "data": data})
    if _loop and not _loop.is_closed():
        asyncio.run_coroutine_threadsafe(broadcast(envelope), _loop)

# ─── WebSocket 핸들러 ────────────────────────────────────
async def ws_handler(ws: WebSocketServerProtocol):
    addr = ws.remote_address
    log.info(f"관제 접속: {addr}")
    connected_ws.add(ws)

    # 접속 즉시 현재 상태 전송
    if last_status:
        snapshot = json.dumps({"type": "snapshot", "data": last_status})
        await ws.send(snapshot)

    try:
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except Exception:
                continue

            # 관제 → 로봇 커맨드 중계
            if msg.get("type") == "cmd":
                robot_id = msg.get("robot_id")
                payload  = msg.get("payload", {})
                if robot_id is not None:
                    topic = MQTT_TOPIC_CMD.format(id=robot_id)
                    mqtt_client.publish(topic, json.dumps(payload))
                    log.info(f"CMD → {topic}: {payload}")

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_ws.discard(ws)
        log.info(f"관제 접속 종료: {addr}")

# ─── 메인 ───────────────────────────────────────────────
async def main():
    global _loop
    _loop = asyncio.get_running_loop()

    # WebSocket 서버 시작
    ws_server = await websockets.serve(ws_handler, WS_HOST, WS_PORT)
    log.info(f"WebSocket 서버 시작: ws://{WS_HOST}:{WS_PORT}")

    try:
        await asyncio.Future()   # 영구 대기
    finally:
        ws_server.close()
        await ws_server.wait_closed()

if __name__ == "__main__":
    # MQTT 클라이언트 시작 (별도 스레드)
    mqtt_client = mqtt.Client(client_id="server_py", clean_session=True)
    mqtt_client.on_connect    = on_connect
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.on_message    = on_message

    mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()   # 백그라운드 스레드

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("종료 중...")
    finally:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()
