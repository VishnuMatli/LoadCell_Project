import socket
import matplotlib.pyplot as plt
import numpy as np
import time
from scipy.signal import firwin, lfilter
import os
import threading
from collections import deque
import struct  # Import struct for packing/unpacking binary data

# Configuration
SERVER_IP = '127.0.0.1'
SERVER_PORT = 9999
BUFFER_SIZE = 4096
SEPARATOR = b'||'
ZERO_CAL = 0.01823035255075
SCALE_CAL = 0.00000451794631


def normalize(values):
    """
    Normalize the raw ADC values to weights using the provided formula.

    Args:
        values (list): List of raw ADC values.

    Returns:
        list: List of calculated weights.
    """
    weights = []
    for raw_value in values:
        try:
            data_in = float(raw_value) / float(0x80000000)
            calculated_weight = (data_in - ZERO_CAL) / SCALE_CAL
            weights.append(calculated_weight)
        except ZeroDivisionError:
            print("[CLIENT] Error: Division by zero in normalize function.")
            return [0.0] * len(values)  # Return a list of zeros
        except Exception as e:
            print(f"[CLIENT] Error in normalize function: {e}")
            return [0.0] * len(values)
    return weights



def remove_dc_offset(values):
    """Remove DC offset by subtracting the mean"""
    try:
        return values - np.mean(values)
    except Exception as e:
        print(f"[CLIENT] Error in remove_dc_offset function: {e}")
        return np.zeros_like(values)


def compute_fft(values, sampling_rate):
    """Compute FFT and return frequency spectrum"""
    N = len(values)
    if N < 2:
        return np.array([0]), np.array([0]), 0
    try:
        windowed_values = values * np.hanning(N)
        fft_values = np.fft.fft(windowed_values)
        fft_magnitude = np.abs(fft_values[:N // 2])
        frequencies = np.fft.fftfreq(N, d=1 / sampling_rate)[:N // 2]
        dominant_freq_index = np.argmax(fft_magnitude)
        cut_off_frequency = frequencies[dominant_freq_index]
        return frequencies, fft_magnitude, cut_off_frequency
    except Exception as e:
        print(f"[CLIENT] Error in compute_fft function: {e}")
        return np.array([0]), np.array([0]), 0



def fir_filter(values, cut_off_frequency, sampling_rate):
    """Apply FIR filter using dynamic cut-off frequency"""
    try:
        nyquist = sampling_rate / 2
        normalized_cutoff = cut_off_frequency / nyquist
        fir_coefficients = firwin(numtaps=51, cutoff=normalized_cutoff, window="hamming", pass_zero=True)
        filtered_values = lfilter(fir_coefficients, 1.0, values)
        return filtered_values, fir_coefficients
    except Exception as e:
        print(f"[CLIENT] Error in fir_filter function: {e}")
        return values, []  # Return original values and empty coefficients


def live_plot(raw_values, filtered_values, interval_ms, label="ADC Data"):
    """Animate raw data, FIR-filtered data, and FFT spectrum dynamically"""
    norm_raw = normalize(raw_values)
    norm_filtered = normalize(filtered_values)

    plt.ion()
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 8), sharex=False)
    fig.subplots_adjust(hspace=0.5)

    # Time-domain plot (Raw Data)
    ax1.set_title(f"Raw ADC Data - {label}")
    ax1.set_xlabel("Sample Index")
    ax1.set_ylabel("Weights")
    ax1.set_xlim(0, max(100, len(norm_raw)))
    ax1.set_ylim(min(norm_raw), max(norm_raw))
    ax1.grid(True)
    raw_line, = ax1.plot([], [], color='red', label="Raw Data")
    ax1.legend()

    # Time-domain plot (FIR-Filtered Data)
    ax2.set_title(f"FIR-Filtered ADC Data - {label}")
    ax2.set_xlabel("Sample Index")
    ax2.set_ylabel("Weights")
    ax2.set_xlim(0, max(100, len(norm_filtered)))
    ax2.set_ylim(min(norm_filtered), max(norm_filtered))
    ax2.grid(True)
    filtered_line, = ax2.plot([], [], color='black', label="FIR-Filtered Data")
    ax2.legend()

    # Frequency-domain plot (FFT)
    ax3.set_title(f"FFT Spectrum - {label}")
    ax3.set_xlabel("Frequency (Hz)")
    ax3.set_ylabel("Magnitude")
    ax3.grid(True)
    fft_line, = ax3.plot([], [], color='blue', label="FFT Spectrum")
    ax3.legend()

    x_data, raw_y_data, filtered_y_data = [], [], []
    fft_x_data, fft_y_data = [], []
    start_time = time.time()
    window_size = 128
    raw_buffer = deque(maxlen=1000)
    filtered_buffer = deque(maxlen=1000)

    for i in range(len(filtered_values)):
        x_data.append(i)
        raw_buffer.append(norm_raw[i + len(norm_raw) - len(filtered_values)])
        filtered_buffer.append(norm_filtered[i])
        raw_y_data = list(raw_buffer)
        filtered_y_data = list(filtered_buffer)

        raw_line.set_data(x_data, raw_y_data)
        filtered_line.set_data(x_data, filtered_y_data)
        ax1.set_xlim(0, max(100, i + 10))
        ax2.set_xlim(0, max(100, i + 10))

        elapsed_time = time.time() - start_time
        actual_sampling_rate = len(filtered_y_data) / elapsed_time if elapsed_time > 0 else 1000 / interval_ms

        fft_frequencies, fft_magnitude, cut_off_frequency = compute_fft(filtered_y_data[-window_size:],
                                                                        actual_sampling_rate)
        fft_x_data = fft_frequencies
        fft_y_data = fft_magnitude
        fft_line.set_data(fft_x_data, fft_y_data)
        if len(fft_frequencies) > 0:
            ax3.set_xlim(0, max(fft_frequencies))
        if len(fft_magnitude) > 0:
            ax3.set_ylim(0, max(fft_magnitude) * 1.2)
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(interval_ms / 1000.0)
    plt.ioff()
    plt.tight_layout()
    plt.show()



def write_data_to_file(file_name, raw_weights, filtered_weights, fir_coefficients, fft_frequencies, fft_magnitude):
    """
    Writes all data to a text file.  Handles potential errors.

    Args:
        file_name (str): The name of the file.
        raw_weights (list): List of raw weight values.
        filtered_weights (list): List of filtered weight values.
        fir_coefficients (list): List of FIR filter coefficients.
        fft_frequencies (list): List of FFT frequencies.
        fft_magnitude (list): List of FFT magnitudes.
    """
    try:
        if not os.path.exists("output_data"):
            os.makedirs("output_data")
        filepath = os.path.join("output_data", f"all_data_{file_name}.txt")
        with open(filepath, "w") as f:
            f.write(f"Raw Weights:\n{raw_weights}\n\n")
            f.write(f"Filtered Weights:\n{filtered_weights}\n\n")
            f.write(f"FIR Coefficients:\n{fir_coefficients}\n\n")
            f.write(f"FFT Frequencies:\n{fft_frequencies}\n\n")
            f.write(f"FFT Magnitudes:\n{fft_magnitude}\n\n")
        print(f"[CLIENT] Successfully wrote data to {filepath}")
    except Exception as e:
        print(f"[CLIENT] Error writing to file: {e}")



def process_data(raw_values, interval_ms, file_name):
    """
    Process the raw data, including normalization, filtering, and FFT.

    Args:
        raw_values (list): List of raw ADC values.
        interval_ms (int): The sampling interval in milliseconds.
        file_name (str): The name of the data file.
    """
    try:
        if len(raw_values) > 0:
            print(f"[CLIENT] Processing data for {file_name}")
            raw_values = remove_dc_offset(np.array(raw_values))
            fft_frequencies, fft_magnitude, cut_off_frequency = compute_fft(raw_values, 1000 / interval_ms)
            filtered_values, fir_coefficients = fir_filter(raw_values, cut_off_frequency, 1000 / interval_ms)
            normalized_raw_values = normalize(raw_values)
            normalized_filtered_values = normalize(filtered_values)

            # Calculate weights
            raw_weights = [(float(val) / float(0x80000000) - ZERO_CAL) / SCALE_CAL for val in raw_values]
            filtered_weights = [(float(val) / float(0x80000000) - ZERO_CAL) / SCALE_CAL for val in filtered_values]

            # Print first 5 values for debugging
            print(f"[CLIENT] Raw Weights (first 5): {raw_weights[:5]}")
            print(f"[CLIENT] Filtered Weights (first 5): {filtered_weights[:5]}")
            print(f"[CLIENT] FIR Coefficients (first 5): {fir_coefficients[:5]}")
            print(f"[CLIENT] FFT Frequencies (first 5): {fft_frequencies[:5]}")
            print(f"[CLIENT] FFT Magnitudes (first 5): {fft_magnitude[:5]}")

            write_data_to_file(file_name, raw_weights, filtered_weights, fir_coefficients, fft_frequencies, fft_magnitude)
            live_plot(normalized_raw_values, normalized_filtered_values, interval_ms, label=file_name)
        else:
            print(f"[CLIENT] No data to process for {file_name}")
    except Exception as e:
        print(f"[CLIENT] Error processing data: {e}")



def receive_and_plot():
    """
    Receives data from the server, processes it, and displays it.
    Handles data reception, parsing, and calls processing functions.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((SERVER_IP, SERVER_PORT))
        print("[CLIENT] Connected to server.")
        buffer = b''
        interval_ms = 20
        mode = "interval"
        file_name = ""

        while True:
            data = s.recv(BUFFER_SIZE)
            if not data:
                break
            buffer += data

            while SEPARATOR in buffer:
                sep_idx = buffer.index(SEPARATOR)
                header = buffer[:sep_idx].decode(errors='ignore')
                buffer = buffer[sep_idx + len(SEPARATOR):]

                if "INTERVAL:" in header:
                    interval_ms = int(header.split(":")[1])
                    print(f"[CLIENT] Received interval: {interval_ms} ms")
                elif "MODE:" in header:
                    mode = header.split(":")[1]
                    print(f"[CLIENT] Received mode: {mode}")
                elif header:
                    file_name = header
                    print(f"[CLIENT] Received file header: {file_name}")
                    file_content = b''
                    print(f"[CLIENT] Receiving file content for {file_name}")
                    while True:
                        next_part = s.recv(BUFFER_SIZE)
                        if not next_part:
                            break
                        if SEPARATOR in next_part:
                            next_sep = next_part.index(SEPARATOR)
                            file_content += next_part[:next_sep]
                            buffer = next_part[next_sep:]
                            break
                        else:
                            file_content += next_part
                    print(f"[CLIENT] Received file content. Length: {len(file_content)}")
                    lines = file_content.decode(errors='ignore').splitlines()
                    raw_values = []
                    for line in lines:
                        if 'ADC:' in line:
                            try:
                                adc = int(line.strip().split("ADC:")[-1])
                                raw_values.append(adc)
                            except ValueError:
                                print(f"[CLIENT] Warning: Invalid ADC value in line: {line}")
                                # Handle the error, e.g., skip this value or use a default
                                pass
                    if raw_values:
                        threading.Thread(target=process_data, args=(raw_values, interval_ms, file_name)).start()
                    else:
                        print(f"[CLIENT] No ADC values found in file content.")

    except ConnectionResetError:
        print("[CLIENT] Error: Connection with server was reset.")
    except ConnectionAbortedError:
        print("[CLIENT] Error: Connection with server was aborted.")
    except Exception as e:
        print(f"[CLIENT] Error: {e}")
    finally:
        s.close()
        print("[CLIENT] Connection closed.")



if __name__ == "__main__":
    receive_and_plot()
