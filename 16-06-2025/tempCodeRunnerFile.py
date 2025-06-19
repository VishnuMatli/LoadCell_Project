import socket
import threading
import os
import time
import tkinter as tk
from tkinter import ttk, filedialog
import numpy as np

# Server Configuration
SERVER_IP = '0.0.0.0'
SERVER_PORT = 9999
BUFFER_SIZE = 4096
DATA_FOLDER = "adc_data"
SEPARATOR = b'||'


def create_sample_data():
    """
    Creates sample ADC data files in the specified folder if they don't exist.  Each file contains 100 lines of "ADC:..." formatted data.
    """
    if not os.path.exists(DATA_FOLDER):
        os.makedirs(DATA_FOLDER)

    for file_index in range(3):
        filename = os.path.join(DATA_FOLDER, f"data_file_{file_index}.txt")
        if not os.path.exists(filename):
            with open(filename, "w") as data_file:
                for _ in range(100):
                    adc_value = int(np.random.rand() * 0x80000000)
                    data_file.write(f"ADC:{adc_value}\n")

    # Create a file specifically for frequency testing (e.g., 50Hz)
    frequency_test_filename = os.path.join(DATA_FOLDER, "data_file_hz50.txt")
    if not os.path.exists(frequency_test_filename):
        with open(frequency_test_filename, "w") as data_file:
            for _ in range(100):
                adc_value = int(np.random.rand() * 0x80000000)
                data_file.write(f"ADC:{adc_value}\n")



class LoadCellServerApp:
    """
    A class representing the Load Cell Server application, built with Tkinter.
    """

    def __init__(self, root):
        """
        Initializes the Load Cell Server application.

        Args:
            root (tk.Tk): The main Tkinter window.
        """
        self.root = root
        self.root.title("Load Cell Server")
        self.root.geometry("600x500")
        self.root.configure(bg="#121212")  # Dark background

        self._setup_ui()
        self._apply_styles()
        self._initialize_variables()
        self._update_tick_marks()  # Initial update of tick marks

    def _setup_ui(self):
        """
        Sets up the user interface elements for the server application.  This includes labels, radio buttons, dropdowns, entry fields, buttons, and a status display.
        """
        # Gradient background frame
        self.gradient_frame = tk.Frame(self.root, bg="#121212")
        self.gradient_frame.place(relwidth=1, relheight=1)

        # Title Label
        self.title_label = tk.Label(self.gradient_frame,
                                     text="Load Cell Server",
                                     font=("Helvetica", 18, "bold"),
                                     fg="#00FFFF",  # Cyan title
                                     bg="#121212")
        self.title_label.pack(pady=10)

        # Mode Selection
        self.mode_label = tk.Label(self.gradient_frame,
                                     text="Select Mode:",
                                     font=("Helvetica", 12),
                                     fg="white",
                                     bg="#121212")
        self.mode_label.pack(pady=5)

        self.mode_var = tk.StringVar(value="interval")  # Default mode

        # Custom-styled radio buttons with tick marks
        self.interval_radio = tk.Radiobutton(
            self.gradient_frame,
            text=" ✔ Send files at fixed intervals",
            variable=self.mode_var,
            value="interval",
            font=("Helvetica", 11),
            fg="white",
            bg="#1E1E1E",  # Dark radio button background
            activebackground="#00A2FF",  # Cyan active background
            indicatoron=0,  # No default indicator
            width=30,
            relief="flat",
            selectcolor="#1E1E1E",  # Keep background on select
            highlightthickness=0,
            command=self._update_tick_marks,
        )
        self.interval_radio.pack()

        self.frequency_radio = tk.Radiobutton(
            self.gradient_frame,
            text="   Send specific file by frequency (Hz)",
            variable=self.mode_var,
            value="freq",
            font=("Helvetica", 11),
            fg="white",
            bg="#1E1E1E",
            activebackground="#00A2FF",
            indicatoron=0,
            width=30,
            relief="flat",
            selectcolor="#1E1E1E",
            highlightthickness=0,
            command=self._update_tick_marks,
        )
        self.frequency_radio.pack()

        self.select_file_radio = tk.Radiobutton(
            self.gradient_frame,
            text="   Select a file and send",
            variable=self.mode_var,
            value="select_file",
            font=("Helvetica", 11),
            fg="white",
            bg="#1E1E1E",
            activebackground="#00A2FF",
            indicatoron=0,
            width=30,
            relief="flat",
            selectcolor="#1E1E1E",
            highlightthickness=0,
            command=self._update_tick_marks,
        )
        self.select_file_radio.pack()

        # Interval Selection
        self.interval_label = tk.Label(self.gradient_frame,
                                         text="Select Sending Interval (ms):",
                                         font=("Helvetica", 12),
                                         fg="white",
                                         bg="#121212")
        self.interval_label.pack(pady=5)

        self.interval_var = tk.StringVar(value="20")  # Default interval
        self.interval_options = ["1", "2", "5", "10", "20", "50", "100"]
        self.interval_dropdown = ttk.Combobox(
            self.gradient_frame,
            textvariable=self.interval_var,
            values=self.interval_options,
            state="readonly",
        )
        self.interval_dropdown.pack()

        # Frequency Selection
        self.frequency_label = tk.Label(self.gradient_frame,
                                          text="Enter Frequency (Hz):",
                                          font=("Helvetica", 12),
                                          fg="white",
                                          bg="#121212")
        self.frequency_label.pack(pady=5)
        self.frequency_entry = ttk.Entry(self.gradient_frame)
        self.frequency_entry.insert(0, "50")  # Default frequency
        self.frequency_entry.pack()

        # File Selection Button
        self.select_file_button = ttk.Button(
            self.gradient_frame,
            text="Select File",
            command=self._select_file,
            style="TButton",
        )
        self.select_file_button.pack(pady=10)

        # Start Button
        self.start_button = ttk.Button(
            self.gradient_frame,
            text="Start Sending",
            command=self._start_sending,
            style="TButton",
        )
        self.start_button.pack(pady=10)

        # Status Label
        self.status_label = tk.Label(self.gradient_frame,
                                      text="",
                                      font=("Helvetica", 12),
                                      fg="#00FF00",  # Green status text
                                      bg="#121212")
        self.status_label.pack()

    def _apply_styles(self):
        """
        Applies custom styles to the widgets using ttk.Style.
        """
        style = ttk.Style()
        style.configure(
            "TButton",
            font=("Helvetica", 12, "bold"),
            padding=6,
            relief="flat",
            background="#00FFFF",  # Cyan button
        )
        style.configure(
            "TEntry",
            font=("Helvetica", 12),
            padding=5,
        )
        style.configure(
            "TCombobox",
            font=("Helvetica", 12),
            padding=5,
        )

    def _initialize_variables(self):
        """
        Initializes variables used by the application.
        """
        self.selected_file = None
        self.client_address = None

    def _update_tick_marks(self):
        """
        Updates the tick marks in the radio buttons to reflect the selected mode.
        """
        selected_mode = self.mode_var.get()
        self.interval_radio.config(
            text=f"{'✔' if selected_mode == 'interval' else ' '}"
            " Send files at fixed intervals"
        )
        self.frequency_radio.config(
            text=f"{'✔' if selected_mode == 'freq' else ' '}"
            " Send specific file by frequency (Hz)"
        )
        self.select_file_radio.config(
            text=f"{'✔' if selected_mode == 'select_file' else ' '}"
            " Select a file and send"
        )

    def _select_file(self):
        """
        Opens a file dialog to allow the user to select a file to send.
        """
        self.selected_file = filedialog.askopenfilename(
            title="Select a File", filetypes=[("Text Files", "*.txt")]
        )
        if self.selected_file:
            filename = os.path.basename(self.selected_file)
            self.status_label.config(text=f"Selected File: {filename}")

    def _start_sending(self):
        """
        Starts the file sending process in a separate thread.
        """
        threading.Thread(target=self._send_files_thread, daemon=True).start()

    def _send_files_thread(self):
        """
        Handles the sending of files to the client, based on the selected mode.
        """
        try:
            server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            server_socket.bind((SERVER_IP, SERVER_PORT))
            server_socket.listen(1)
            self.status_label.config(text="Waiting for client connection...")
            connection, client_address = server_socket.accept()
            self.client_address = client_address
            self.status_label.config(text=f"Connected to {client_address}")

            all_files = sorted(os.listdir(DATA_FOLDER))
            print(f"[SERVER] Files in {DATA_FOLDER}: {all_files}")

            selected_mode = self.mode_var.get()
            if selected_mode == "interval":
                self._send_files_at_interval(connection, all_files)
            elif selected_mode == "freq":
                self._send_file_by_frequency(connection, all_files)
            elif selected_mode == "select_file":
                self._send_selected_file(connection)

            connection.close()
            self.status_label.config(text="Finished sending files.")
        except Exception as error:
            self.status_label.config(text=f"Error: {str(error)}")
            print(f"[SERVER] Exception in _send_files_thread: {error}")

    def _send_files_at_interval(self, connection, all_files):
        """
        Sends all text files in the data folder at a fixed interval to the client.

        Args:
            connection (socket.socket): The socket connection to the client.
            all_files (list): A list of all files in the data folder.
        """
        interval_ms = int(self.interval_var.get())
        connection.sendall(f"INTERVAL:{interval_ms}".encode() + SEPARATOR)
        connection.sendall(f"MODE:interval".encode() + SEPARATOR)
        for filename in all_files:
            if filename.endswith(".txt"):
                print(f"[SERVER] Sending file: {filename}")
                self._send_file(connection, os.path.join(DATA_FOLDER, filename))
                time.sleep(interval_ms / 1000.0)

    def _send_file_by_frequency(self, connection, all_files):
        """
        Sends a specific file based on the entered frequency.

        Args:
            connection (socket.socket): The socket connection to the client.
             all_files (list): A list of all files in the data folder.
        """
        frequency = self.frequency_entry.get()
        matched_files = [
            filename
            for filename in all_files
            if f"hz{frequency}" in filename and filename.endswith(".txt")
        ]

        if not matched_files:
            self.status_label.config(text=f"No file found for {frequency} Hz")
            connection.sendall(
                f"NO_FILE_FOUND:{frequency}".encode() + SEPARATOR
            )
            connection.close()
            return

        connection.sendall(
            f"INTERVAL:20".encode() + SEPARATOR
        )  # Default interval for frequency mode
        connection.sendall(f"MODE:freq".encode() + SEPARATOR)
        self._send_file(
            connection, os.path.join(DATA_FOLDER, matched_files[0]), 20
        )  # Hardcoded interval

    def _send_selected_file(self, connection):
        """
        Sends the file selected by the user.

        Args:
            connection (socket.socket): The socket connection to the client.
        """
        if not self.selected_file:
            self.status_label.config(text="No file selected!")
            connection.close()
            return

        connection.sendall(f"INTERVAL:20".encode() + SEPARATOR)  # Default
        connection.sendall(f"MODE:select_file".encode() + SEPARATOR)
        self._send_file(connection, self.selected_file, 20)  # Hardcoded interval

    def _send_file(self, connection, filepath, interval_ms=20):
        """
        Sends the contents of a file to the client.

        Args:
            connection (socket.socket): The socket connection to the client.
            filepath (str): The path to the file to send.
            interval_ms (int): interval in milliseconds
        """
        try:
            with open(filepath, "rb") as file:
                file_data = file.read()
            filename_header = os.path.basename(filepath).encode() + SEPARATOR
            print(f"[SERVER] Sending header: {filename_header}")
            connection.sendall(filename_header + file_data)
        except Exception as error:
            print(f"[SERVER] Error in _send_file: {error}")
            self.status_label.config(f"Error sending file : {error}")



if __name__ == "__main__":
    create_sample_data()  # Ensure sample data exists
    root = tk.Tk()
    app = LoadCellServerApp(root)
    root.mainloop()
