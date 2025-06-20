import socket
import threading
import os
import time
import tkinter as tk
from tkinter import ttk, filedialog
import numpy as np

# Server config
SERVER_IP = '0.0.0.0'
SERVER_PORT = 9999
DATA_FOLDER = "adc_data"
BUFFER_SIZE = 4096
FILENAME_LENGTH_BYTES = 4
FILE_CONTENT_LENGTH_BYTES = 8
CONFIG_LENGTH_BYTES = 4

def create_sample_data():
    if not os.path.exists(DATA_FOLDER):
        os.makedirs(DATA_FOLDER)
    # 3 generic files
    for i in range(3):
        fn = os.path.join(DATA_FOLDER, f"data_file_{i}.txt")
        if not os.path.exists(fn):
            with open(fn, "w") as f:
                for _ in range(100):
                    f.write(f"ADC:{int(np.random.rand()*0x80000000)}\n")
    # frequency-specific
    fn = os.path.join(DATA_FOLDER, "data_file_hz50.txt")
    if not os.path.exists(fn):
        with open(fn, "w") as f:
            for _ in range(100):
                f.write(f"ADC:{int(np.random.rand()*0x80000000)}\n")

class LoadCellServerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Load Cell Server")
        self.root.geometry("600x500")
        self.root.configure(bg="#121212")
        create_sample_data()
        self._setup_ui()
        self._apply_styles()
        self._initialize_vars()
        self._update_tick_marks()
        self.root.protocol("WM_DELETE_WINDOW", self._on_closing)

    def _setup_ui(self):
        f = tk.Frame(self.root, bg="#121212"); f.place(relwidth=1, relheight=1)
        tk.Label(f, text="Load Cell Server", font=("Helvetica",18,"bold"),
                 fg="#00FFFF", bg="#121212").pack(pady=10)
        tk.Label(f, text="Select Mode:", font=("Helvetica",12),
                 fg="white", bg="#121212").pack(pady=5)
        self.mode_var = tk.StringVar(value="interval")
        self.interval_radio = tk.Radiobutton(f, text=" ✔ Send files at fixed intervals",
                                             variable=self.mode_var, value="interval",
                                             font=("Helvetica",11), fg="white",
                                             bg="#1E1E1E", activebackground="#00A2FF",
                                             indicatoron=0, width=30, relief="flat",
                                             selectcolor="#1E1E1E",
                                             command=self._update_tick_marks)
        self.interval_radio.pack()
        self.frequency_radio = tk.Radiobutton(f, text="   Send specific file by frequency (Hz)",
                                              variable=self.mode_var, value="freq",
                                              font=("Helvetica",11), fg="white",
                                              bg="#1E1E1E", activebackground="#00A2FF",
                                              indicatoron=0, width=30, relief="flat",
                                              selectcolor="#1E1E1E",
                                              command=self._update_tick_marks)
        self.frequency_radio.pack()
        self.select_file_radio = tk.Radiobutton(f, text="   Select a file and send",
                                                variable=self.mode_var, value="select_file",
                                                font=("Helvetica",11), fg="white",
                                                bg="#1E1E1E", activebackground="#00A2FF",
                                                indicatoron=0, width=30, relief="flat",
                                                selectcolor="#1E1E1E",
                                                command=self._update_tick_marks)
        self.select_file_radio.pack()
        tk.Label(f, text="Select Sending Interval (ms):",
                 font=("Helvetica",12), fg="white", bg="#121212").pack(pady=5)
        self.interval_var = tk.StringVar(value="20")
        self.interval_options = ["1","2","5","10","20","50","100"]
        self.interval_dropdown = ttk.Combobox(f, textvariable=self.interval_var,
                                              values=self.interval_options, state="readonly")
        self.interval_dropdown.pack()
        tk.Label(f, text="Enter Frequency (Hz):",
                 font=("Helvetica",12), fg="white", bg="#121212").pack(pady=5)
        self.frequency_entry = ttk.Entry(f); self.frequency_entry.insert(0, "50")
        self.frequency_entry.pack()
        self.select_file_button = ttk.Button(f, text="Select File", command=self._select_file,
                                             style="TButton")
        self.select_file_button.pack(pady=10)
        self.start_button = ttk.Button(f, text="Start Sending", command=self._start_sending,
                                       style="TButton")
        self.start_button.pack(pady=10)
        self.stop_button = ttk.Button(f, text="Stop Sending", command=self._stop_sending,
                                      style="TButton", state="disabled")
        self.stop_button.pack(pady=5)
        self.status_label = tk.Label(f, text="", font=("Helvetica",12),
                                     fg="#00FF00", bg="#121212")
        self.status_label.pack()

    def _apply_styles(self):
        s = ttk.Style()
        s.configure("TButton", font=("Helvetica",12,"bold"), padding=6,
                    relief="flat", background="#00FFFF")
        s.map("TButton", foreground=[('active','black'),('disabled','grey')],
              background=[('active','#00DDDD'),('disabled','#006666')])
        s.configure("TEntry", font=("Helvetica",12), padding=5)
        s.configure("TCombobox", font=("Helvetica",12), padding=5)

    def _initialize_vars(self):
        self.selected_file = None
        self.server_socket = None
        self.connection = None
        self.is_sending = False
        self.send_thread = None

    def _update_tick_marks(self):
        m = self.mode_var.get()
        self.interval_radio.config(text=("✔" if m=="interval" else " ")+" Send files at fixed intervals")
        self.frequency_radio.config(text=("✔" if m=="freq" else " ")+" Send specific file by frequency (Hz)")
        self.select_file_radio.config(text=("✔" if m=="select_file" else " ")+" Select a file and send")
        self.interval_dropdown.config(state="readonly" if m=="interval" else "disabled")
        self.frequency_entry.config(state="normal" if m=="freq" else "disabled")
        self.select_file_button.config(state="normal" if m=="select_file" else "disabled")

    def _select_file(self):
        fn = filedialog.askopenfilename(title="Select a File", filetypes=[("Text Files","*.txt")])
        if fn:
            self.selected_file = fn
            self.status_label.config(text=f"Selected File: {os.path.basename(fn)}")
        else:
            self.status_label.config(text="No file selected.")

    def _start_sending(self):
        if self.is_sending:
            self.status_label.config(text="Already sending...")
            return
        self._cleanup()
        self.is_sending = True
        self.start_button.config(state="disabled")
        self.stop_button.config(state="normal")
        self.send_thread = threading.Thread(target=self._send_files_thread, daemon=True)
        self.send_thread.start()

    def _stop_sending(self):
        if not self.is_sending:
            self.status_label.config(text="Not currently sending.")
            return
        self.is_sending = False
        self.status_label.config(text="Stopping sending...")
        self.stop_button.config(state="disabled")
        self.start_button.config(state="normal")
        # Allow thread to notice flag
        self._cleanup()

    def _on_closing(self):
        self._stop_sending()
        self.root.destroy()

    def _cleanup(self):
        try:
            if self.connection:
                self.connection.shutdown(socket.SHUT_RDWR); self.connection.close()
        except: pass
        self.connection = None
        try:
            if self.server_socket:
                self.server_socket.shutdown(socket.SHUT_RDWR); self.server_socket.close()
        except: pass
        self.server_socket = None

    def _send_files_thread(self):
        try:
            self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_socket.bind((SERVER_IP, SERVER_PORT))
            self.server_socket.listen(1)
            self.server_socket.settimeout(1)
            self.status_label.config(text="Waiting for client connection...")
            while self.is_sending:
                try:
                    self.connection, addr = self.server_socket.accept()
                    break
                except socket.timeout:
                    continue
            if not self.is_sending:
                self.status_label.config(text="Sending cancelled.")
                return
            self.status_label.config(text=f"Connected to {addr}")
            files = sorted([f for f in os.listdir(DATA_FOLDER) if f.endswith(".txt")])
            interval_ms = int(self.interval_var.get())
            mode = self.mode_var.get()
            self._send_config(self.connection, interval_ms, mode)
            if mode == "interval":
                self._send_files_at_interval(files, interval_ms)
            elif mode == "freq":
                self._send_file_by_frequency(files)
            else:
                self._send_selected_file()
            if self.is_sending:
                self.status_label.config(text="Finished sending.")
        except Exception as e:
            self.status_label.config(text=f"Error: {e}")
        finally:
            self.is_sending = False
            self._cleanup()
            self.root.after(100, lambda: self.start_button.config(state="normal"))
            self.root.after(100, lambda: self.stop_button.config(state="disabled"))

    def _send_config(self, conn, interval_ms, mode):
        cfg = f"INTERVAL:{interval_ms}\nMODE:{mode}\n".encode('utf-8')
        conn.sendall(len(cfg).to_bytes(CONFIG_LENGTH_BYTES,'big') + cfg)

    def _send_files_at_interval(self, files, interval_ms):
        for f in files:
            if not self.is_sending: break
            self._send_file(self.connection, os.path.join(DATA_FOLDER,f))
            time.sleep(interval_ms/1000.0)
        if self.is_sending:
            self._send_file(self.connection, None)

    def _send_file_by_frequency(self, files):
        freq = self.frequency_entry.get()
        matched = [f for f in files if f"hz{freq}" in f]
        if not matched:
            msg = f"NO_FILE_FOUND:{freq}".encode()
            self.connection.sendall(len(msg).to_bytes(FILENAME_LENGTH_BYTES,'big')+msg+
                                    (0).to_bytes(FILE_CONTENT_LENGTH_BYTES,'big'))
        else:
            self._send_file(self.connection, os.path.join(DATA_FOLDER,matched[0]))
            self._send_file(self.connection, None)

    def _send_selected_file(self):
        if not self.selected_file:
            msg = b"NO_FILE_SELECTED"
            self.connection.sendall(len(msg).to_bytes(FILENAME_LENGTH_BYTES,'big')+msg+
                                    (0).to_bytes(FILE_CONTENT_LENGTH_BYTES,'big'))
        else:
            self._send_file(self.connection, self.selected_file)
            self._send_file(self.connection, None)

    def _send_file(self, conn, filepath):
        if filepath is None:
            fn = "END_OF_TRANSMISSION".encode('utf-8'); data = b""
        else:
            fn = os.path.basename(filepath).encode('utf-8')
            with open(filepath, "rb") as f: data = f.read()
        conn.sendall(len(fn).to_bytes(FILENAME_LENGTH_BYTES,'big'))
        conn.sendall(fn)
        conn.sendall(len(data).to_bytes(FILE_CONTENT_LENGTH_BYTES,'big'))
        conn.sendall(data)

if __name__ == "__main__":
    root = tk.Tk()
    app = LoadCellServerApp(root)
    root.mainloop()
