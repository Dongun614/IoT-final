# app_controller.py
class AppController:
    def __init__(self, root, model, socket_ctrl, view):
        self.root = root
        self.model = model
        self.socket_ctrl = socket_ctrl
        self.view = view

    # ------------------------
    def connect_server(self, idx):
        return self.socket_ctrl.connect_server(idx)

    def disconnect_server(self, idx):
        self.socket_ctrl.disconnect(idx)

    def run_stop_command(self, idx):
        self.socket_ctrl.send_motor(idx, 0, 0)

    # ------------------------
    #  TIMED FORWARD MOTION
    # ------------------------
    def run_timed_forward(self, idx, pwm, duration):
        # forward: 양쪽 동일 PWM
        self.socket_ctrl.send_motor(idx, pwm, pwm)

        # duration 이후 자동 stop
        self.root.after(int(duration * 1000), lambda: self.run_stop_command(idx))
