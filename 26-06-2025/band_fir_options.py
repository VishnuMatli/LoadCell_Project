# File: band_fir.py
# FINAL version with corrected real-time filter logic.

import socket
import matplotlib.pyplot as plt
from matplotlib.widgets import RadioButtons
import numpy as np
import time
from scipy.signal import firwin
import os
import threading
from collections import deque
import struct
import queue

try:
    import cmsisdsp as dsp
    version_string = f"v{dsp.__version__}" if hasattr(dsp, '__version__') else "(version unknown)"
    print(f"[CLIENT] CMSIS-DSP library {version_string} loaded successfully.")
except ImportError:
    print("[CLIENT] ERROR: CMSIS-DSP library not found. Please install it using 'pip install cmsisdsp'")
    exit()

# A thread-safe event to signal shutdown to all threads
shutdown_event = threading.Event()

# Default plotting speed, will be updated by the widget
USER_SELECTED_INTERVAL_MS = 20

# Configuration
SERVER_IP = '127.0.0.1'
SERVER_PORT = 9999
FIR_CUTOFF_HZ = 10.0

# Protocol constants
FILENAME_LENGTH_BYTES = 4
FILE_CONTENT_LENGTH_BYTES = 8
CONFIG_LENGTH_BYTES = 4

# Calibration constants
ZERO_CAL = 0.01823035255075
SCALE_CAL = 0.00000451794631

# Global variables
current_raw_buffer = deque(maxlen=500)
current_filtered_buffer = deque(maxlen=500)
dsp_raw_adc_buffer = deque(maxlen=500)
current_fig, current_axes = None, None
current_raw_line, current_filtered_line = None, None
plot_lock = threading.Lock()
speed_radio_buttons = None


def calculate_amplitudes_and_dc(raw_adc_values, sampling_rate):
    """
    Calculates amplitudes and the STABLE DC offset for the entire signal.
    """
    print("[CLIENT] Calculating signal amplitudes and stable DC offset...")
    if not raw_adc_values:
        return None, None, 0

    try:
        signal_as_f32 = np.array(raw_adc_values, dtype=np.float32)
        stable_dc_offset = np.mean(signal_as_f32)
        print(f"[CLIENT] Stable DC offset calculated: {stable_dc_offset:.2f}")

        # Calculate raw amplitude
        raw_weights = normalize_to_weights(raw_adc_values)
        raw_amplitude = np.nanmax(raw_weights) - np.nanmin(raw_weights)
        print(f"[CLIENT] Raw data amplitude (peak-to-peak): {raw_amplitude:.2f}")
        
        # Calculate filtered amplitude using the stable offset
        dc_removed_signal = signal_as_f32 - stable_dc_offset
        filtered_ac_signal, _ = fir_filter(dc_removed_signal, FIR_CUTOFF_HZ, sampling_rate)

        if filtered_ac_signal.size > 0 and not np.all(np.isnan(filtered_ac_signal)):
            reconstructed_filtered_signal = filtered_ac_signal + stable_dc_offset
            filtered_weights = normalize_to_weights(reconstructed_filtered_signal)
            filtered_amplitude = np.nanmax(filtered_weights) - np.nanmin(filtered_weights)
            print(f"[CLIENT] Filtered data amplitude (peak-to-peak): {filtered_amplitude:.2f}")
        else:
            filtered_amplitude = None
            print("[CLIENT] Could not generate filtered signal to calculate amplitude.")
            
        return raw_amplitude, filtered_amplitude, stable_dc_offset
        
    except Exception as e:
        print(f"[CLIENT] Error during amplitude calculation: {e}")
        return None, None, 0

def save_amplitude_results(original_file_name, raw_amplitude, filtered_amplitude):
    """Saves the amplitude calculation results to a unique file."""
    output_directory = "analysis_results"
    
    try:
        if not os.path.exists(output_directory):
            os.makedirs(output_directory)
            print(f"[CLIENT] Created output directory: {output_directory}")

        output_filename = f"analysis_{original_file_name}"
        output_filepath = os.path.join(output_directory, output_filename)
        
        with open(output_filepath, "w") as f:
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            raw_amp_str = f"{raw_amplitude:.4f}" if raw_amplitude is not None else "N/A"
            filt_amp_str = f"{filtered_amplitude:.4f}" if filtered_amplitude is not None else "N/A"
            f.write(f"Analysis for file: {original_file_name}\n")
            f.write(f"Analysis timestamp: {timestamp}\n")
            f.write("-" * 30 + "\n")
            f.write(f"Raw Data Peak-to-Peak Amplitude: {raw_amp_str}\n")
            f.write(f"Filtered Data Peak-to-Peak Amplitude: {filt_amp_str}\n")
            
        print(f"[CLIENT] Amplitude results saved to: {output_filepath}")
        
    except Exception as e:
        print(f"[CLIENT] Error saving amplitude to file: {e}")


def normalize_to_weights(values):
    """Normalize raw ADC values to physical weights."""
    weights = []
    for val in values:
        try:
            data_in = float(val) / float(0x80000000)
            calculated_weight = (data_in - ZERO_CAL) / SCALE_CAL if SCALE_CAL != 0 else 0.0
            weights.append(calculated_weight)
        except (ValueError, TypeError):
            weights.append(np.nan)
    return np.array(weights)


def fir_filter(values, cut_off_frequency, sampling_rate):
    """Apply FIR low-pass filter using the 'flat' API of CMSIS-DSP v1.10.1."""
    try:
        nyquist = sampling_rate / 2
        numtaps = 51
        if nyquist <= 0 or cut_off_frequency <= 0 or cut_off_frequency >= nyquist or numtaps > len(values):
            return np.full_like(values, np.nan), np.array([], dtype=np.float32)

        normalized_cutoff = cut_off_frequency / nyquist
        fir_coefficients = firwin(numtaps=numtaps, cutoff=normalized_cutoff, window="hamming", pass_zero=True)
        fir_coeffs_f32 = np.array(fir_coefficients, dtype=np.float32)

        block_size = len(values)
        state_f32 = np.zeros(numtaps + block_size - 1, dtype=np.float32)
        fir_instance = dsp.arm_fir_instance_f32()
        dsp.arm_fir_init_f32(fir_instance, numtaps, fir_coeffs_f32, state_f32)
        filtered_values = dsp.arm_fir_f32(fir_instance, values)
        return filtered_values, fir_coeffs_f32
    except Exception as e:
        print(f"[CLIENT] Error in fir_filter (CMSIS-DSP): {e}")
        return np.full_like(values, np.nan), np.array([], dtype=np.float32)


def init_plot(label="ADC Data"):
    """Initializes the Matplotlib figure, axes, and interactive widgets."""
    global current_fig, current_axes, current_raw_line, current_filtered_line, speed_radio_buttons
    with plot_lock:
        plt.close('all')
        current_fig, current_axes = plt.subplots(2, 1, figsize=(10, 6))
        
        plt.subplots_adjust(left=0.25)

        current_raw_line, = current_axes[0].plot([], [], 'r-', label="Raw Data")
        current_filtered_line, = current_axes[1].plot([], [], 'k-', label=f"FIR-Filtered @ {FIR_CUTOFF_HZ}Hz (CMSIS-DSP)")
        
        current_axes[0].set_title(f"Raw ADC Data - {label}")
        current_axes[0].set_ylabel("Weight")
        current_axes[0].grid(True)
        current_axes[0].legend(loc='upper right')
        
        current_axes[1].set_title(f"FIR-Filtered Data - {label}")
        current_axes[1].set_xlabel("Sample Index")
        current_axes[1].set_ylabel("Weight")
        current_axes[1].grid(True)
        current_axes[1].legend(loc='upper right')
        
        widget_ax = plt.axes([0.03, 0.4, 0.15, 0.25])
        speed_options = ["1ms", "2ms", "5ms", "10ms", "20ms", "50ms", "100ms"]
        
        speed_radio_buttons = RadioButtons(widget_ax, speed_options, active=4)

        def on_speed_change(label):
            global USER_SELECTED_INTERVAL_MS
            try:
                new_interval = int(label.replace('ms', ''))
                USER_SELECTED_INTERVAL_MS = new_interval
                print(f"[CLIENT] Real-time speed changed to: {new_interval}ms interval.")
            except ValueError:
                pass
        
        speed_radio_buttons.on_clicked(on_speed_change)
        current_fig.canvas.mpl_connect('close_event', lambda evt: shutdown_event.set())
        print(f"[CLIENT] Plot initialized for: {label}. Close the plot window to exit.")


def update_live_plot():
    """Updates the Matplotlib plots using global data buffers."""
    if shutdown_event.is_set() or current_fig is None: return
    with plot_lock:
        try:
            raw_data = np.array(list(current_raw_buffer))
            filtered_data = np.array(list(current_filtered_buffer))
            current_raw_line.set_data(np.arange(len(raw_data)), raw_data)
            current_filtered_line.set_data(np.arange(len(filtered_data)), filtered_data)
            current_axes[0].set_xlim(0, current_raw_buffer.maxlen)
            current_axes[1].set_xlim(0, current_raw_buffer.maxlen)

            if raw_data.size > 0 and np.any(np.isfinite(raw_data)):
                min_y, max_y = np.nanmin(raw_data), np.nanmax(raw_data)
                padding = (max_y - min_y) * 0.1 or 0.1
                current_axes[0].set_ylim(min_y - padding, max_y + padding)
            if filtered_data.size > 0 and np.any(np.isfinite(filtered_data)):
                min_y, max_y = np.nanmin(filtered_data), np.nanmax(filtered_data)
                padding = (max_y - min_y) * 0.1 or 0.1
                current_axes[1].set_ylim(min_y - padding, max_y + padding)
                
            current_fig.canvas.draw_idle()
            current_fig.canvas.flush_events()
        except Exception as e:
            if "FigureManagerBase" not in str(e):
                 print(f"[CLIENT] Error during plot update: {e}")


def process_and_plot_live_data(raw_adc_values, file_name):
    """Calculates amplitudes and DC offset, saves them, then simulates live plotting."""
    if not raw_adc_values:
        print(f"[CLIENT] No data to process for {file_name}.")
        return

    # --- RECTIFIED: Use a fixed sampling rate for ALL calculations based on source data ---
    # We assume the server's initial interval is the true sampling rate of the data
    source_data_interval = 20.0 # Default if not received
    # This can be improved by passing the interval from the network thread
    source_sampling_rate = 1000.0 / source_data_interval
    
    raw_amp, filtered_amp, stable_dc_offset = calculate_amplitudes_and_dc(raw_adc_values, source_sampling_rate)
    save_amplitude_results(file_name, raw_amp, filtered_amp)

    print(f"[CLIENT] Starting live simulation for {file_name}...")
    init_plot(label=file_name)
    num_taps = 51
    min_dsp_samples = num_taps
    
    for adc_value in raw_adc_values:
        if shutdown_event.is_set():
            print("[CLIENT] Plotting stopped by user.")
            break
        
        # --- RECTIFIED: User selection ONLY affects the pause duration ---
        current_interval_ms = USER_SELECTED_INTERVAL_MS
        
        dsp_raw_adc_buffer.append(adc_value)
        raw_weight = normalize_to_weights([adc_value])[0]
        current_raw_buffer.append(raw_weight)
        
        filtered_weight = np.nan
        if len(dsp_raw_adc_buffer) >= min_dsp_samples:
            dsp_window = np.array(list(dsp_raw_adc_buffer)[-min_dsp_samples:], dtype=np.float32)
            
            processed_window = dsp_window - stable_dc_offset
            
            # --- RECTIFIED: Always use the source data's sampling rate for the filter ---
            filtered_ac, _ = fir_filter(processed_window, FIR_CUTOFF_HZ, source_sampling_rate)
            
            if filtered_ac.size > 0 and not np.all(np.isnan(filtered_ac)):
                reconstructed_signal = filtered_ac[-1] + stable_dc_offset
                filtered_weight = normalize_to_weights([reconstructed_signal])[0]
        
        current_filtered_buffer.append(filtered_weight)
        update_live_plot()
        # The pause is the only thing that changes in real-time
        plt.pause(current_interval_ms / 1000.0)

    print(f"[CLIENT] Finished simulation for {file_name}.")


def recvall(sock, n):
    """Helper function to receive n bytes or return None on EOF."""
    data = b''
    while len(data) < n:
        if shutdown_event.is_set(): return None
        try:
            packet = sock.recv(n - len(data))
            if not packet: return None
            data += packet
        except socket.timeout:
            continue
    return data


def receive_data_loop(data_queue):
    """Network thread to connect, receive data, and put it in a queue."""
    print("[CLIENT] Network thread started.")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1.0)
            s.connect((SERVER_IP, SERVER_PORT))
            print("[CLIENT] Connected to server.")
            config_len_bytes = recvall(s, CONFIG_LENGTH_BYTES)
            if not config_len_bytes: raise ConnectionError("Server disconnected.")
            config_len = int.from_bytes(config_len_bytes, 'big')
            recvall(s, config_len)
            print("[CLIENT] Received and ignored server config.")

            while not shutdown_event.is_set():
                filename_len_bytes = recvall(s, FILENAME_LENGTH_BYTES)
                if not filename_len_bytes:
                    print("[CLIENT] Server disconnected or no more files.")
                    break
                filename_length = int.from_bytes(filename_len_bytes, 'big')
                filename_bytes = recvall(s, filename_length)
                if not filename_bytes: break
                file_name = filename_bytes.decode('utf-8')
                if file_name == "END_OF_TRANSMISSION":
                    print("[CLIENT] Received END_OF_TRANSMISSION signal.")
                    break
                file_content_len_bytes = recvall(s, FILE_CONTENT_LENGTH_BYTES)
                if not file_content_len_bytes: break
                file_content_length = int.from_bytes(file_content_len_bytes, 'big')
                file_content_data = recvall(s, file_content_length)
                if not file_content_data: break
                lines = file_content_data.decode(errors='ignore').splitlines()
                raw_adc_values = []
                for line in lines:
                    if 'ADC:' in line:
                        try:
                            adc_val = int(line.strip().split("ADC:")[-1])
                            raw_adc_values.append(adc_val)
                        except (ValueError, IndexError):
                            pass
                if raw_adc_values:
                    data_queue.put((raw_adc_values, file_name))
    except ConnectionRefusedError:
        print("[CLIENT] Connection refused. Is the server running?")
    except Exception as e:
        if not shutdown_event.is_set():
            print(f"[CLIENT] Network thread error: {type(e).__name__} - {repr(e)}")
    finally:
        print("[CLIENT] Network thread finished.")
        data_queue.put(None)


if __name__ == "__main__":
    plt.ion()
    data_queue = queue.Queue()
    network_thread = threading.Thread(target=receive_data_loop, args=(data_queue,), daemon=True)
    network_thread.start()
    
    print("[CLIENT] Main thread waiting for data...")
    try:
        while not shutdown_event.is_set():
            try:
                data_packet = data_queue.get(timeout=0.1)
                if data_packet is None:
                    print("[CLIENT] All files received and processed.")
                    break
                
                raw_adc_values, file_name = data_packet
                
                current_raw_buffer.clear()
                current_filtered_buffer.clear()
                dsp_raw_adc_buffer.clear()
                
                process_and_plot_live_data(raw_adc_values, file_name)

            except queue.Empty:
                continue
            except Exception as e:
                print(f"[CLIENT] Error in main processing loop: {e}")
                shutdown_event.set()
    except KeyboardInterrupt:
        print("\n[CLIENT] Keyboard interrupt received. Shutting down.")
    finally:
        shutdown_event.set()
        print("[CLIENT] Cleaning up...")
        network_thread.join(timeout=2.0)
        plt.ioff()
        if current_fig:
             print("[CLIENT] Displaying final plot. Close window to exit.")
             plt.show()
        print("[CLIENT] Application exited.")
