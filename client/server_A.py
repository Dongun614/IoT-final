#!/usr/bin/env python3
"""
server.py — 통합 수신기

Wi-Fi 경로 (UDP) + LoRa 경로 (UART) 를 동시에 수신해서 하나의 스트림으로 출력.
각 패킷에 channel 필드로 어느 경로로 왔는지 표시.
같은 seq가 양쪽으로 오면 중복(dedup) 처리.
"""

import threading
import socket
import json
import struct
import sys
import argparse
import os
from datetime import datetime
from collections import deque
import time

try:
    import serial
except ImportError:
    print("ERROR: pip install pyserial", file=sys.stderr)
    sys.exit(1)

PAYLOAD_FMT  = '<BHIBbh'   # robot_payload_t: u8 u16 u32 u8 i8 i16
PAYLOAD_SIZE = struct.calcsize(PAYLOAD_FMT)
STATE_NAME   = {0: 'NORMAL', 1: 'WARNING', 2: 'LORA'}

# Dedup: (robot_id, seq) → 수신 시각.  200ms 윈도우
_dedup_lock  = threading.Lock()
_dedup_cache = {}   # (robot_id, seq) → timestamp

def _is_dup(robot_id, seq):
    key = (robot_id, seq)
    now = time.time()
    with _dedup_lock:
        # 오래된 항목 제거
        expired = [k for k, t in _dedup_cache.items() if now - t > 0.5]
        for k in expired:
            del _dedup_cache[k]
        if key in _dedup_cache:
            return True
        _dedup_cache[key] = now
        return False

_print_lock = threading.Lock()
_last_seq   = {}   # robot_id → last seq (gap 감지용)

def print_packet(robot_id, seq, ts, state, rssi, temp_raw,
                 channel, lora_rssi=None, snr=None, tag=None):
    if _is_dup(robot_id, seq):
        return

    prev = _last_seq.get(robot_id, seq - 1)
    gap  = seq - prev - 1
    _last_seq[robot_id] = seq

    ts_str = datetime.fromtimestamp(ts).strftime('%H:%M:%S') if ts else '?'
    out = {
        'seq':        seq,
        'ts':         ts_str,
        'robot_id':   robot_id,
        'state':      STATE_NAME.get(state, '?'),
        'robot_rssi': rssi,
        'temp':       round(temp_raw / 100.0, 2),
        'channel':    channel,
    }
    if lora_rssi is not None:
        out['lora_rssi'] = lora_rssi
        out['snr']       = snr
    if tag:
        out['tag'] = tag
    if gap > 0:
        out['gap'] = gap

    with _print_lock:
        print(json.dumps(out))
        sys.stdout.flush()
        if gap > 0:
            print(f'[GAP!] robot={robot_id} seq={seq} gap={gap}', file=sys.stderr)


def wifi_thread(port):
    """UDP 수신 — Wi-Fi 경로"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(1.0)
    print(f'[WiFi] UDP :{port} 대기중', file=sys.stderr)

    while True:
        try:
            data, _ = sock.recvfrom(1024)
        except socket.timeout:
            continue
        try:
            msg = json.loads(data.decode('utf-8'))
            print_packet(
                robot_id = msg.get('robot_id', 0),
                seq      = msg['seq'],
                ts       = msg.get('ts', 0),
                state    = {'NORMAL':0,'WARNING':1,'LORA':2}.get(msg.get('state',''),0),
                rssi     = msg.get('rssi', 0),
                temp_raw = int(msg.get('temp', 0) * 100),
                channel  = 'wifi',
                tag      = msg.get('tag'),
            )
        except Exception as e:
            print(f'[WiFi] 파싱 오류: {e}  raw={data[:80]}', file=sys.stderr)


def lora_thread(port, baud):
    """UART 수신 — LoRa 경로"""
    print(f'[LoRa] {port} @ {baud} 열기중...', file=sys.stderr)
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except Exception as e:
        print(f'[LoRa] 포트 오류: {e}', file=sys.stderr)
        return
    print(f'[LoRa] 대기중', file=sys.stderr)

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
            print(f'[LoRa RAW] {line}', file=sys.stderr)
            continue

        if msg.get('type') != 'rx':
            print(f'[LoRa TTGO] {msg}', file=sys.stderr)
            continue

        try:
            data = bytes.fromhex(msg['data'])
            robot_id, seq, ts, state, rssi, temp_raw = struct.unpack_from(
                PAYLOAD_FMT, data, 0)
            print_packet(
                robot_id  = robot_id,
                seq       = seq,
                ts        = ts,
                state     = state,
                rssi      = rssi,
                temp_raw  = temp_raw,
                channel   = 'lora',
                lora_rssi = msg.get('rssi'),
                snr       = msg.get('snr'),
            )
        except Exception as e:
            print(f'[LoRa] 디코드 오류: {e}', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description='통합 서버 (Wi-Fi UDP + LoRa UART)')
    parser.add_argument('--wifi-port',  type=int, default=int(os.environ.get('WIFI_PORT', 5000)))
    parser.add_argument('--lora-port',  default=os.environ.get('SERVER_PORT',
                            '/dev/tty.wchusbserial58DD0208991'))
    parser.add_argument('--lora-baud',  type=int, default=115200)
    args = parser.parse_args()

    wt = threading.Thread(target=wifi_thread, args=(args.wifi_port,),  daemon=True, name='wifi')
    lt = threading.Thread(target=lora_thread, args=(args.lora_port, args.lora_baud), daemon=True, name='lora')

    wt.start()
    lt.start()

    print(f'[server] Wi-Fi UDP :{args.wifi_port}  /  LoRa {args.lora_port}', file=sys.stderr)
    print('[server] Ctrl+C로 종료', file=sys.stderr)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print('\n[server] 종료', file=sys.stderr)


if __name__ == '__main__':
    main()
