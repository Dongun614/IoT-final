# socket_controller.py
import socket
import threading
import time

class SocketController:
    def __init__(self, model, view):
        self.model = model
        self.view = view

        # 소켓 4개
        self.sockets = [None] * model.bot_count

    # ------------------------
    #  BOT CONNECT
    # ------------------------
    def connect_server(self, idx):
        ip = self.model.server_ips[idx]
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3)
            sock.connect((ip, 12345))
            self.sockets[idx] = sock
            self.model.is_connected[idx] = True
            return True
        except Exception as e:
            self.view.log_message(f"[BOT {idx+1}] CONNECT ERROR: {e}")
            self.model.is_connected[idx] = False
            return False

    # ------------------------
    def disconnect(self, idx):
        if self.sockets[idx]:
            try:
                self.sockets[idx].close()
            except:
                pass
        self.sockets[idx] = None
        self.model.is_connected[idx] = False

    # ------------------------
    #  SEND MOTOR COMMAND
    # ------------------------
    def send_motor(self, idx, left_pwm, right_pwm):
        if not self.model.is_connected[idx]:
            self.view.log_message(f"[BOT {idx+1}] Not connected")
            return
        try:
            msg = f"M,{left_pwm},{right_pwm}\n".encode()
            self.sockets[idx].sendall(msg)
        except Exception as e:
            self.view.log_message(f"[BOT {idx+1}] SEND FAIL: {e}")
            self.disconnect(idx)
