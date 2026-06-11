import tkinter as tk
from tkinter import ttk
import time
import math

from app_model import AppModel
from app_controller import AppController
from socket_controller import SocketController


BOT_COUNT = 4   # 라즈봇 최대 개수


class App:
    def __init__(self, root, model):
        self.root = root
        self.model = model
        self.controller = None

        self.root.title("Raspbot Multi Control (Up to 4)")
        self.root.geometry("850x650")

        # UI 요소 저장 구조
        self.bot_frames = []
        self.ip_entries = []
        self.pwm_entries = []
        self.duration_entries = []
        self.connect_buttons = []
        self.start_buttons = []
        self.stop_buttons = []

        self._create_widgets()

        # style
        style = ttk.Style()
        style.configure('Connected.TButton', foreground='green', font=('Arial', 10, 'bold'))
        style.configure('Disconnected.TButton', foreground='red', font=('Arial', 10, 'normal'))

    def set_controller(self, controller):
        self.controller = controller
        self._bind_commands()

    #-----------------------------------------
    #                 UI
    #-----------------------------------------
    def _create_widgets(self):
        main_frame = ttk.Frame(self.root, padding=10)
        main_frame.pack(fill="both", expand=True)

        # -----------------------------------
        #   4개 BOT 패널 생성
        # -----------------------------------
        bots_frame = ttk.LabelFrame(main_frame, text="Raspberry Pi Bots")
        bots_frame.pack(fill="x", pady=10)

        for i in range(BOT_COUNT):
            bot_frame = ttk.LabelFrame(bots_frame, text=f"BOT {i+1}", padding=10)
            bot_frame.grid(row=i//2, column=i%2, padx=10, pady=10, sticky="nsew")

            # IP
            ttk.Label(bot_frame, text="IP:").grid(row=0, column=0, pady=5)
            ip_entry = ttk.Entry(bot_frame, width=15)
            ip_entry.grid(row=0, column=1, pady=5)
            self.ip_entries.append(ip_entry)

            # Connect Button
            connect_btn = ttk.Button(bot_frame, text="Connect")
            connect_btn.grid(row=0, column=2, padx=5)
            self.connect_buttons.append(connect_btn)

            # PWM Input
            ttk.Label(bot_frame, text="PWM:").grid(row=1, column=0, pady=5)
            pwm_entry = ttk.Entry(bot_frame, width=10)
            pwm_entry.insert(0, "150")
            pwm_entry.grid(row=1, column=1, pady=5)
            self.pwm_entries.append(pwm_entry)

            # Duration
            ttk.Label(bot_frame, text="Duration (s):").grid(row=2, column=0)
            dur_entry = ttk.Entry(bot_frame, width=10)
            dur_entry.insert(0, "2")
            dur_entry.grid(row=2, column=1)
            self.duration_entries.append(dur_entry)

            # Start & Stop Buttons
            start_btn = ttk.Button(bot_frame, text="START")
            start_btn.grid(row=3, column=0, columnspan=2, pady=5)
            self.start_buttons.append(start_btn)

            stop_btn = ttk.Button(bot_frame, text="STOP")
            stop_btn.grid(row=3, column=2, pady=5)
            self.stop_buttons.append(stop_btn)

        #-----------------------------------------
        #  Dead Reckoning Status & Log
        #-----------------------------------------
        dr_frame = ttk.LabelFrame(main_frame, text="Dead Reckoning (Unified)")
        dr_frame.pack(fill="both", expand=True, pady=10)

        self.dr_status_label = ttk.Label(dr_frame, text="DR Status...", font=('Arial', 14))
        self.dr_status_label.pack(pady=10)

        log_frame = ttk.LabelFrame(main_frame, text="Log")
        log_frame.pack(fill="both", expand=True)

        self.log_text = tk.Text(log_frame, height=10)
        self.log_text.pack(fill="both", expand=True)

    #-----------------------------------------
    #            COMMAND BINDING
    #-----------------------------------------
    def _bind_commands(self):

        for i in range(BOT_COUNT):
            bot_index = i

            # connect
            self.connect_buttons[i].config(
                command=lambda idx=bot_index: self._connect_bot(idx)
            )
            # start movement
            self.start_buttons[i].config(
                command=lambda idx=bot_index: self._start_bot_motion(idx)
            )
            # stop
            self.stop_buttons[i].config(
                command=lambda idx=bot_index: self.controller.run_stop_command(idx)
            )

    #-----------------------------------------
    #             BOT ACTIONS
    #-----------------------------------------

    def _connect_bot(self, idx):
        ip = self.ip_entries[idx].get().strip()
        if ip == "":
            self.log_message(f"[BOT {idx+1}] IP is empty!")
            return

        # model에 저장
        self.model.server_ips[idx] = ip

        # connect attempt
        connected = self.controller.connect_server(idx)

        if connected:
            self.connect_buttons[idx].config(
                text="Disconnect",
                style="Connected.TButton",
                command=lambda i=idx: self._disconnect_bot(i)
            )
            self.log_message(f"[BOT {idx+1}] Connected ({ip})")
        else:
            self.log_message(f"[BOT {idx+1}] Connection Failed ({ip})")

    def _disconnect_bot(self, idx):
        self.controller.disconnect_server(idx)

        self.connect_buttons[idx].config(
            text="Connect",
            style="Disconnected.TButton",
            command=lambda i=idx: self._connect_bot(i)
        )
        self.log_message(f"[BOT {idx+1}] Disconnected")

    def _start_bot_motion(self, idx):
        try:
            pwm = int(self.pwm_entries[idx].get())
            duration = float(self.duration_entries[idx].get())
        except ValueError:
            self.log_message(f"[BOT {idx+1}] Invalid PWM or Duration")
            return

        self.log_message(f"[BOT {idx+1}] Movement Start (PWM={pwm}, {duration}s)")

        self.controller.run_timed_forward(idx, pwm, duration)

    #-----------------------------------------
    def update_dr_status(self):
        dr = self.model.dead_reckoner
        txt = f"X={dr.X_POS:.3f}, Y={dr.Y_POS:.3f}, θ={math.degrees(dr.THETA):.1f}°"
        self.dr_status_label.config(text=txt)

    def log_message(self, msg):
        self.log_text.insert(tk.END, f"[{time.strftime('%H:%M:%S')}] {msg}\n")
        self.log_text.see(tk.END)


# ===========================================
#                   MAIN
# ===========================================
if __name__ == "__main__":
    root = tk.Tk()
    model = AppModel(bot_count=BOT_COUNT)
    view = App(root, model)
    socket_ctrl = SocketController(model, view)
    controller = AppController(root, model, socket_ctrl, view)

    view.set_controller(controller)

    def on_close():
        for i in range(BOT_COUNT):
            controller.disconnect_server(i)
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()
