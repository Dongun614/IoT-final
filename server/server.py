#!/usr/bin/env python3
"""
server.py — 통합 수신기 + 관제 WebSocket 서버

Wi-Fi 경로 (UDP) + LoRa 경로 (UART) 를 동시에 수신.
수신 데이터를 WebSocket으로 관제 브라우저(index.html)에 브로드캐스트.
관제 브라우저의 커맨드(CMD)를 로봇 RPi로 UDP 전달.

설치: pip install websockets pyserial
실행: python3 server.py
      python3 server.py --wifi-port 5000 --lora-port /dev/lora_server --ws-port 8765
"""

import asyncio
import threading
import socket
import json
import struct
import sys
import argparse
import os
import time
import logging
from datetime import datetime

import websockets
from websockets.server import WebSocketServerProtocol

try:
    import serial
except ImportError:
    print("ERROR: pip install pyserial", file=sys.stderr)
    sys.exit(1)

# ─── 설정 ───────────────────────────────────────────────
WIFI_PORT       = int(os.environ.get('WIFI_PORT', 5000))
LORA_PORT       = os.environ.get('LORA_PORT', '/dev/lora_server')
LORA_BAUD       = 115200
WS_HOST         = '0.0.0.0'
WS_PORT         = 8765
DEDUP_WINDOW_S  = 0.2    # 200ms 중복 제거 윈도우

# CMD 역방향: 서버가 로봇 RPi에게 UDP로 전달
# 로봇 RPi C 코드가 이 포트를 수신 대기해야 함
CMD_DEST_PORT   = 5001

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
log = logging.getLogger('server')

# ─── 페이로드 구조체 포맷 ───────────────────────────────
PAYLOAD_FMT  = '<BHIBbh'   # robot_payload_t: u8 u16 u32 u8 i8 i16
PAYLOAD_SIZE = struct.calcsize(PAYLOAD_FMT)
STATE_NUM    = {'NORMAL': 0, 'WARNING': 1, 'LORA': 2}
STATE_NAME   = {0: 'NORMAL', 1: 'WARNING', 2: 'LORA_ONLY'}

# ─── 공유 상태 ──────────────────────────────────────────
connected_ws: set[WebSocketServerProtocol] = set()
last_status:  dict[int, dict] = {}        # robot_id → 마지막 상태 (스냅샷용)
robot_addrs:  dict[int, str]  = {}        # robot_id → IP (CMD 역방향용)

# asyncio 루프 참조 (스레드 → asyncio 브리지)
_loop: asyncio.AbstractEventLoop | None = None

# ─── dedup ──────────────────────────────────────────────
_dedup_lock  = threading.Lock()
_dedup_cache: dict[tuple, float] = {}

def _is_dup(robot_id: int, seq: int) -> bool:
    key = (robot_id, seq)
    now = time.time()
    with _dedup_lock:
        expired = [k for k, t in _dedup_cache.items() if now - t > DEDUP_WINDOW_S * 10]
        for k in expired:
            del _dedup_cache[k]
        if key in _dedup_cache:
            return True
        _dedup_cache[key] = now
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

def broadcast_from_thread(data: dict):
    """스레드(wifi/lora)에서 asyncio broadcast 호출"""
    if _loop and not _loop.is_closed():
        msg = json.dumps({'type': 'status', 'data': data})
        asyncio.run_coroutine_threadsafe(broadcast(msg), _loop)

# ─── 패킷 처리 공통 ─────────────────────────────────────
_last_seq: dict[int, int] = {}

def handle_packet(robot_id, seq, ts, state, wifi_rssi, temp_raw,
                  channel, lora_rssi=None, lora_snr=None, src_ip=None):
    """dedup → 필드 정규화 → last_status 저장 → WebSocket 브로드캐스트"""
    if _is_dup(robot_id, seq):
        log.debug(f'중복 패킷 무시: robot={robot_id} seq={seq}')
        return

    # seq gap 감지
    prev_seq = _last_seq.get(robot_id, seq - 1)
    gap = seq - prev_seq - 1
    _last_seq[robot_id] = seq
    if gap > 0:
        log.warning(f'[GAP] robot={robot_id} seq={seq} gap={gap}')

    # 로봇 IP 기록 (CMD 역방향용)
    if src_ip:
        robot_addrs[robot_id] = src_ip

    # index.html이 기대하는 필드명으로 정규화
    data = {
        'robot_id':  robot_id,
        'seq':       seq,
        'ts':        ts,
        'state':     state,          # 숫자 (0/1/2)
        'wifi_rssi': wifi_rssi,      # index.html: d.wifi_rssi
        'temp':      round(temp_raw / 100.0, 2),
        'channel':   channel,
    }
    if lora_rssi is not None:
        data['lora_rssi'] = lora_rssi   # index.html: d.lora_rssi
        data['lora_snr']  = lora_snr    # index.html: d.lora_snr
    if gap > 0:
        data['gap'] = gap

    last_status[robot_id] = data

    log.info(
        f'robot={robot_id} seq={seq} ch={channel} '
        f'state={STATE_NAME.get(state,"?")} '
        f'wifi_rssi={wifi_rssi} lora_rssi={lora_rssi} temp={data["temp"]}'
    )

    broadcast_from_thread(data)

# ─── Wi-Fi 수신 스레드 (UDP) ────────────────────────────
def wifi_thread(port: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(1.0)
    log.info(f'[WiFi] UDP :{port} 대기중')

    while True:
        try:
            raw, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue
        try:
            msg = json.loads(raw.decode('utf-8'))
            handle_packet(
                robot_id  = msg.get('robot_id', 0),
                seq       = msg['seq'],
                ts        = msg.get('ts', 0),
                state     = STATE_NUM.get(msg.get('state', ''), msg.get('state', 0)),
                wifi_rssi = msg.get('rssi', 0),
                temp_raw  = int(msg.get('temp', 0) * 100),
                channel   = 'wifi',
                src_ip    = addr[0],
            )
        except Exception as e:
            log.warning(f'[WiFi] 파싱 오류: {e}  raw={raw[:80]}')

# ─── LoRa 수신 스레드 (UART) ────────────────────────────
def lora_thread(port: str, baud: int):
    log.info(f'[LoRa] {port} @ {baud} 열기중...')
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except Exception as e:
        log.error(f'[LoRa] 포트 오류: {e}')
        return
    log.info('[LoRa] 대기중')

    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode('utf-8', errors='replace').strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except Exception:
            log.debug(f'[LoRa RAW] {line}')
            continue

        if msg.get('type') == 'ready':
            log.info(f'[LoRa] TTGO 준비 완료: {msg}')
            continue
        if msg.get('type') == 'error':
            log.error(f'[LoRa] TTGO 오류: {msg}')
            continue
        if msg.get('type') != 'rx':
            log.debug(f'[LoRa TTGO] {msg}')
            continue

        try:
            data = bytes.fromhex(msg['data'])
            robot_id, seq, ts, state, wifi_rssi, temp_raw = struct.unpack_from(
                PAYLOAD_FMT, data, 0)
            handle_packet(
                robot_id  = robot_id,
                seq       = seq,
                ts        = ts,
                state     = state,
                wifi_rssi = wifi_rssi,
                temp_raw  = temp_raw,
                channel   = 'lora',
                lora_rssi = msg.get('rssi'),
                lora_snr  = msg.get('snr'),
            )
        except Exception as e:
            log.warning(f'[LoRa] 디코드 오류: {e}')

# ─── CMD 역방향 처리 ────────────────────────────────────
def send_cmd_to_robot(robot_id: int, payload: dict):
    """
    관제 브라우저 → 로봇 RPi UDP 전달.
    로봇 RPi C 코드가 CMD_DEST_PORT(5001)를 수신 대기해야 함.
    """
    ip = robot_addrs.get(robot_id)
    if not ip:
        log.warning(f'CMD 전달 실패: robot={robot_id} IP 미등록 (Wi-Fi 패킷 수신 전)')
        return
    try:
        msg = json.dumps({'robot_id': robot_id, **payload})
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(msg.encode(), (ip, CMD_DEST_PORT))
        sock.close()
        log.info(f'CMD → {ip}:{CMD_DEST_PORT} robot={robot_id} {payload}')
    except Exception as e:
        log.error(f'CMD 전송 오류: {e}')

# ─── WebSocket 핸들러 ────────────────────────────────────
async def ws_handler(ws: WebSocketServerProtocol):
    addr = ws.remote_address
    log.info(f'관제 접속: {addr}')
    connected_ws.add(ws)

    # 접속 즉시 현재 상태 스냅샷 전송
    if last_status:
        snapshot = json.dumps({'type': 'snapshot', 'data': last_status})
        await ws.send(snapshot)
        log.info(f'스냅샷 전송: {len(last_status)}대')

    try:
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except Exception:
                continue

            # 관제 → 로봇 커맨드
            if msg.get('type') == 'cmd':
                robot_id = msg.get('robot_id')
                payload  = msg.get('payload', {})
                if robot_id is not None:
                    log.info(f'CMD 수신: robot={robot_id} {payload}')
                    send_cmd_to_robot(robot_id, payload)

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_ws.discard(ws)
        log.info(f'관제 접속 종료: {addr}')

# ─── 메인 ───────────────────────────────────────────────
async def main(args):
    global _loop
    _loop = asyncio.get_running_loop()

    ws_server = await websockets.serve(ws_handler, WS_HOST, args.ws_port)
    log.info(f'WebSocket 서버 시작: ws://{WS_HOST}:{args.ws_port}')

    try:
        await asyncio.Future()
    finally:
        ws_server.close()
        await ws_server.wait_closed()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='통합 서버 (Wi-Fi UDP + LoRa UART + WebSocket)')
    parser.add_argument('--wifi-port', type=int, default=WIFI_PORT)
    parser.add_argument('--lora-port', default=LORA_PORT)
    parser.add_argument('--lora-baud', type=int, default=LORA_BAUD)
    parser.add_argument('--ws-port',   type=int, default=WS_PORT)
    args = parser.parse_args()

    # Wi-Fi / LoRa 수신 스레드 시작
    wt = threading.Thread(target=wifi_thread, args=(args.wifi_port,),
                          daemon=True, name='wifi')
    lt = threading.Thread(target=lora_thread, args=(args.lora_port, args.lora_baud),
                          daemon=True, name='lora')
    wt.start()
    lt.start()

    log.info(f'Wi-Fi UDP :{args.wifi_port}  /  LoRa {args.lora_port}  /  WS :{args.ws_port}')
    log.info('종료: Ctrl+C')

    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        log.info('종료 중...')