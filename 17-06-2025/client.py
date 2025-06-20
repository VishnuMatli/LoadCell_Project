import socket
import threading
import time
import os
from collections import deque
import matplotlib.pyplot as plt
import numpy as np
from scipy.signal import firwin, lfilter

# ======================= Configuration =======================
SERVER_IP = '127.0.0.1'
SERVER_PORT = 9999

FILENAME_LENGTH_BYTES = 4
FILE_CONTENT_LENGTH_BYTES = 8
CONFIG_LENGTH_BYTES = 4

# Calibration constants
ZERO_CAL = 0.01823035255075
SCALE_CAL = 0.00000451794631

# Buffer length for live plotting
BUFFER_SIZE = 100

# Flags
stop_receiving_data = False

# ================================= Utility Functions =================================

def recvall(sock, n):
    data = b''
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data += packet
    return data

def normalize(adc_values):
    weights = []
    for v in adc_values:
        data_in = float(v) / float(0x80000000)
        weights.append((data_in - ZERO_CAL) / SCALE_CAL)
    return weights

# ================================= Data Processing =================================

def compute_fft(values, sampling_rate):
    N = len(values)
    if N < 2:
        return np.array([]), np.array([]), 0
    windowed = values * np.hanning(N)
    fft_res = np.fft.fft(windowed)
    fft_mag = np.abs(fft_res[:N // 2])
    freqs = np.fft.fftfreq(N, d=1/sampling_rate)[:N // 2]
    dominant_idx = np.argmax(fft_mag) if fft_mag.size else 0
    cutoff = freqs[dominant_idx] if freqs.size else 0
    return freqs, fft_mag, cutoff

def apply_fir_filter(values, cutoff_frequency, sampling_rate):
    nyquist = sampling_rate / 2
    if cutoff_frequency <= 0 or cutoff_frequency >= nyquist:
        return values, []
    normalized_cutoff = cutoff_frequency / nyquist
    coeffs = firwin(numtaps=51, cutoff=normalized_cutoff, window='hamming')
    filtered = lfilter(coeffs, 1.0, values)
    return filtered, coeffs

# Save to text file
def write_data_to_file(file_name, raw_w, filt_w, fir_coeffs, freqs, fft_mag):
    os.makedirs("output_data", exist_ok=True)
    path = os.path.join("output_data", f"all_data_{file_name}.txt")
    with open(path, "w") as f:
        f.write("Raw Weights:\n")
        f.write(np.array2string(np.array(raw_w), max_line_width=np.inf))
        f.write("\n\nFiltered Weights:\n")
        f.write(np.array2string(np.array(filt_w), max_line_width=np.inf))
        f.write("\n\nFIR Filter Coefficients:\n")
        f.write(np.array2string(np.array(fir_coeffs), max_line_width=np.inf))
        f.write("\n\nFFT Frequencies:\n")
        f.write(np.array2string(np.array(freqs), max_line_width=np.inf))
        f.write("\n\nFFT Magnitudes:\n")
        f.write(np.array2string(np.array(fft_mag), max_line_width=np.inf))

# ================================= Live Plotting Setup =================================

def init_live_plot():
    plt.ion()
    fig, (ax_raw, ax_filt, ax_fft) = plt.subplots(3, 1, figsize=(10, 8))
    line_raw, = ax_raw.plot([], [], color='red', label="Raw Data")
    line_filt, = ax_filt.plot([], [], color='black', label="FIR-Filtered Data")
    line_fft, = ax_fft.plot([], [], color='blue', label="FFT Magnitude")

    ax_raw.set_title("Raw Weight vs. Time")
    ax_filt.set_title("Filtered Weight vs. Time")
    ax_fft.set_title("FFT Spectrum")

    for ax in (ax_raw, ax_filt, ax_fft):
        ax.set_xlabel("Sample Index")
        ax.set_ylabel("Amplitude")
        ax.grid(True)
        ax.legend()

    fig.tight_layout()
    return fig, ax_raw, ax_filt, ax_fft, line_raw, line_filt, line_fft

# ================================= Plot Loop =================================

def update_live_plot(fig, ax_raw, ax_filt, ax_fft, line_raw, line_filt, line_fft,
                     raw_buffer, filt_buffer, fft_freqs, fft_mag, sampling_rate):
    N = len(raw_buffer)
    x = np.arange(N) * (1.0 / sampling_rate)  # time axis in seconds

    line_raw.set_data(x, raw_buffer)
    line_filt.set_data(x, filt_buffer)

    ax_raw.set_xlim(0, x[-1] if N > 1 else 1)
    ax_raw.set_ylim(min(raw_buffer), max(raw_buffer))

    ax_filt.set_xlim(0, x[-1] if N > 1 else 1)
    ax_filt.set_ylim(min(filt_buffer), max(filt_buffer))

    line_fft.set_data(fft_freqs, fft_mag)
    ax_fft.set_xlim(0, max(fft_freqs) if fft_freqs.size > 0 else 1)
    ax_fft.set_ylim(0, max(fft_mag) if fft_mag.size > 0 else 1)

    fig.canvas.draw()
    fig.canvas.flush_events()

# ================================= Data Receiving Thread =================================

def receive_and_plot_data_loop():
    global stop_receiving_data

    # Initial buffers
    raw_buffer = deque(maxlen=BUFFER_SIZE)
    filt_buffer = deque(maxlen=BUFFER_SIZE)
    for _ in range(BUFFER_SIZE):
        raw_buffer.append(0.0)
        filt_buffer.append(0.0)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((SERVER_IP, SERVER_PORT))
    sock.settimeout(1.0)

    # Receive config
    cfg_len_bytes = recvall(sock, CONFIG_LENGTH_BYTES)
    if not cfg_len_bytes:
        print("[CLIENT] No config received.")
        return
    cfg_len = int.from_bytes(cfg_len_bytes, 'big')
    cfg_data = recvall(sock, cfg_len).decode()
    print(f"[CLIENT] Received config:\n{cfg_data}")

    interval_ms = 20
    for line in cfg_data.splitlines():
        if line.startswith("INTERVAL:"):
            try:
                interval_ms = int(line.split(":")[1])
            except ValueError:
                pass

    sampling_rate = 1000.0 / interval_ms

    # Initialize plot
    fig, ax_raw, ax_filt, ax_fft, line_raw, line_filt, line_fft = init_live_plot()

    while not stop_receiving_data:
        filename_len_bytes = recvall(sock, FILENAME_LENGTH_BYTES)
        if not filename_len_bytes:
            break
        fname_len = int.from_bytes(filename_len_bytes, 'big')
        fname = recvall(sock, fname_len).decode()

        content_len_bytes = recvall(sock, FILE_CONTENT_LENGTH_BYTES)
        if not content_len_bytes:
            break
        content_len = int.from_bytes(content_len_bytes, 'big')
        content_data = recvall(sock, content_len).decode(errors='ignore')

        if fname.startswith("NO_FILE"):
            print(f"[CLIENT] Server message: {fname}")
            break
        if fname == "END_OF_TRANSMISSION":
            print("[CLIENT] End of transmission.")
            break

        adc_values = []
        for line in content_data.splitlines():
            if 'ADC:' in line:
                try:
                    adc_values.append(int(line.split("ADC:")[-1]))
                except ValueError:
                    pass

        if not adc_values:
            continue

        samples = len(adc_values)
        for i, adc in enumerate(adc_values):
            weight = (adc / float(0x80000000) - ZERO_CAL) / SCALE_CAL
            raw_buffer.append(weight)

            # running buffer to apply filter & FFT
            current_raw = np.array(raw_buffer)
            freqs, mag, cutoff = compute_fft(current_raw, sampling_rate)
            filtered_values, fir_coeffs = apply_fir_filter(current_raw, cutoff, sampling_rate)
            filt_buffer.append(filtered_values[-1])

            # full FIR-buffer for overall plot
            full_filtered = np.array(filt_buffer)
            fft_freqs = freqs
            fft_mag = mag

            update_live_plot(fig, ax_raw, ax_filt, ax_fft,
                             line_raw, line_filt, line_fft,
                             np.array(raw_buffer),
                             full_filtered,
                             fft_freqs, fft_mag,
                             sampling_rate)

            time.sleep(interval_ms / 1000.0)

        write_data_to_file(fname, list(raw_buffer), list(filt_buffer), fir_coeffs.tolist(), fft_freqs, fft_mag)

    sock.close()
    stop_receiving_data = True
    print("[CLIENT] Receiver thread ending, closing.")

if __name__ == "__main__":
    threading.Thread(target=receive_and_plot_data_loop, daemon=True).start()
    try:
        while not stop_receiving_data:
            time.sleep(0.1)
    except KeyboardInterrupt:
        stop_receiving_data = True
    finally:
        plt.close('all')
        print("[CLIENT] Exiting cleanly.")
