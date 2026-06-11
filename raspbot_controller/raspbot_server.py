# raspbot_server.py (라즈베리파이에서 실행)
import socket
import threading
import json
from YB_Pcb_Car import YB_Pcb_Car

SERVER_IP = '0.0.0.0'
SERVER_PORT = 12345        # PC와 동일 포트 사용
BUFFER_SIZE = 1024

car = YB_Pcb_Car()
car.Car_Stop()


def process_packet(packet):
    """
    PC에서 전송하는 커맨드 형식:
    - M,left_pwm,right_pwm    → 모터 제어
    - S                       → STOP
    - SERVO,id,angle          → 서보 제어
    """

    parts = packet.split(",")

    # ---- Motor control ----
    if parts[0] == "M":
        if len(parts) != 3:
            return "ERR,INVALID_M_FORMAT"

        try:
            left = int(parts[1])
            right = int(parts[2])
        except ValueError:
            return "ERR,INVALID_PWM"

        car.Control_Car(left, right)
        return f"OK,M,{left},{right}"

    # ---- STOP ----
    elif parts[0] == "S":
        car.Car_Stop()
        return "OK,STOP"

    # ---- SERVO ----
    elif parts[0] == "SERVO":
        if len(parts) != 3:
            return "ERR,INVALID_SERVO_FORMAT"

        try:
            servo_id = int(parts[1])
            angle = int(parts[2])
        except ValueError:
            return "ERR,INVALID_SERVO"

        car.Ctrl_Servo(servo_id, angle)
        return f"OK,SERVO,{servo_id},{angle}"

    else:
        return "ERR,UNKNOWN_CMD"


def handle_client(conn, addr):
    print(f"[SERVER] 연결 수립: {addr}")

    try:
        while True:
            data = conn.recv(BUFFER_SIZE)
            if not data:
                break

            packet = data.decode().strip()
            print(f"[RX] {packet}")

            response = process_packet(packet)

            conn.sendall((response + "\n").encode())

    except Exception as e:
        print(f"[SERVER] ERROR: {e}")

    finally:
        car.Car_Stop()
        conn.close()
        print(f"[SERVER] 연결 종료: {addr}")


def start_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        s.bind((SERVER_IP, SERVER_PORT))
        s.listen(5)
        print(f"[SERVER] Raspbot Server Running on {SERVER_PORT}")

        while True:
            conn, addr = s.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()

    finally:
        s.close()
        car.Car_Stop()


if __name__ == "__main__":
    start_server()
