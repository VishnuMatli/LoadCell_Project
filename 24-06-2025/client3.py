import socket
import matplotlib.pyplot as plt
import numpy as np
import time
from scipy.signal import firwin, lfilter
import os
import threading
from collections import deque
import struct # For packing/unpacking binary data
import queue # For thread-safe data passing between network and plotting threads

# Configuration
SERVER_IP = '127.0.0.1'
SERVER_PORT = 9999
BUFFER_SIZE = 4096

# Define the same length byte constants as the server for protocol consistency
FILENAME_LENGTH_BYTES = 4
FILE_CONTENT_LENGTH_BYTES = 8
CONFIG_LENGTH_BYTES = 4

# Calibration constants for converting ADC values to meaningful weights
ZERO_CAL = 0.01823035255075
SCALE_CAL = 0.00000451794631

# Global variables for plotting (managed by the main thread)
current_raw_buffer = deque(maxlen=500) # Buffer for raw data points for live plot (adjust size as needed)
current_filtered_buffer = deque(maxlen=500) # Buffer for filtered data points for live plot
dsp_raw_adc_buffer = deque(maxlen=500) # Buffer to hold raw values (before weight conversion) for DSP operations

# Matplotlib figure and axes objects for live update
current_fig = None 
current_ax1 = None
current_ax2 = None
current_ax3 = None
current_raw_line = None
current_filtered_line = None
current_fft_line = None

plot_lock = threading.Lock() # Protect Matplotlib calls from concurrent access

# Flag to signal plotting activity status
plotting_active = False
network_thread_running = False # Flag to track network thread status


def normalize_to_weights(values):
    """
    Normalize raw ADC values to physical weights using calibration constants.
    This is for plotting and saving final results.
    Assumes `values` are in the original ADC range or re-offset to it.

    Args:
        values (list or np.array): List or array of raw ADC values (or values in that range).

    Returns:
        np.array: Array of calculated weights.
    """
    weights = []
    for val in values:
        try:
            # Ensure value is treated as a float
            data_in = float(val) / float(0x80000000)
            calculated_weight = (data_in - ZERO_CAL) / SCALE_CAL if SCALE_CAL != 0 else 0.0
            weights.append(calculated_weight)
        except Exception as e:
            # print(f"[CLIENT] Error in normalize_to_weights function for value {val}: {e}") # Too verbose for live
            weights.append(np.nan) # Append NaN on error to avoid breaking plot limits
    return np.array(weights)


def remove_dc_offset_temp(values):
    """
    Temporarily remove DC offset by subtracting the mean of the input values.
    This is used *before* FFT/FIR to process AC components.
    """
    try:
        values_array = np.array(values, dtype=float) 
        if values_array.size == 0:
            return np.array([])
        return values_array - np.nanmean(values_array) # Use nanmean to handle potential NaNs
    except Exception as e:
        print(f"[CLIENT] Error in remove_dc_offset_temp function: {e}")
        return np.full_like(values_array, np.nan) # Fill with NaN on error


def compute_fft(values, sampling_rate):
    """
    Compute FFT and return frequency spectrum, magnitude, and dominant frequency.
    `values` are expected to be DC-removed for meaningful AC component analysis.

    Args:
        values (np.array): Array of time-domain signal values (DC-removed).
        sampling_rate (float): Sampling rate of the signal in Hz.

    Returns:
        tuple: (frequencies, fft_magnitude, dominant_frequency)
    """
    N = len(values)
    if N < 2: 
        # print(f"[CLIENT - FFT] Not enough data for FFT (N={N}). Returning zeros.") # Too verbose
        return np.array([0]), np.array([0]), 0.0
    try:
        # print(f"[CLIENT - FFT] Computing FFT for {N} samples (first 5: {values[:5]}). Input mean: {np.nanmean(values):.2f}") # DEBUG: Show input to FFT
        
        windowed_values = values * np.hanning(N)
        fft_values = np.fft.fft(windowed_values)
        fft_magnitude = np.abs(fft_values[:N // 2])
        frequencies = np.fft.fftfreq(N, d=1 / sampling_rate)[:N // 2]
        
        dominant_frequency = 0.0
        if len(fft_magnitude) > 1: 
            search_magnitudes = fft_magnitude[1:] 
            search_frequencies = frequencies[1:]
            if len(search_magnitudes) > 0:
                dominant_freq_index_relative = np.nanargmax(search_magnitudes) # Use nanargmax
                dominant_frequency = search_frequencies[dominant_freq_index_relative]
            else: 
                dominant_frequency = frequencies[0] 
        elif len(fft_magnitude) == 1: 
            dominant_frequency = frequencies[0] if len(frequencies) > 0 else 0.0
        
        # print(f"[CLIENT - FFT] Input Length: {N}, Sampling Rate: {sampling_rate:.2f} Hz")
        # print(f"[CLIENT - FFT] Dominant Freq (excl 0Hz): {dominant_frequency:.2f} Hz, Max Magnitude: {np.nanmax(fft_magnitude):.2e}") # Debug print
        # print(f"[CLIENT - FFT] Frequencies (first 5): {frequencies[:5]}") # DEBUG
        # print(f"[CLIENT - FFT] Magnitudes (first 5): {fft_magnitude[:5]}") # DEBUG
        return frequencies, fft_magnitude, dominant_frequency
    except Exception as e:
        print(f"[CLIENT] Error in compute_fft function: {e}")
        return np.array([0]), np.array([0]), 0.0


def fir_filter(values, cut_off_frequency, sampling_rate):
    """
    Apply FIR low-pass filter using a dynamically determined cut-off frequency.
    `values` are expected to be DC-removed for effective filtering of AC components.

    Args:
        values (np.array): Array of signal values (DC-removed).
        cut_off_frequency (float): The determined cut-off frequency in Hz.
        sampling_rate (float): Sampling rate of the signal in Hz.

    Returns:
        np.array: Array of filtered signal values.
        np.array: Array of FIR filter coefficients.
    """
    try:
        nyquist = sampling_rate / 2
        
        # print(f"[CLIENT - FIR] Input values length: {len(values)}, Sampling Rate: {sampling_rate:.2f} Hz, Nyquist: {nyquist:.2f} Hz")
        # print(f"[CLIENT - FIR] Calculated Cut-off Freq: {cut_off_frequency:.2f} Hz")

        # Ensure cut_off_frequency is valid and normalized_cutoff is within (0, 1)
        if nyquist == 0 or cut_off_frequency <= 0 or cut_off_frequency >= nyquist:
            # print(f"[CLIENT - FIR] Warning: Invalid cut-off frequency ({cut_off_frequency:.2f} Hz) or Nyquist frequency ({nyquist:.2f} Hz).")
            if nyquist > 0:
                if cut_off_frequency >= nyquist: 
                    normalized_cutoff = 0.99 
                elif cut_off_frequency <= 0:
                    normalized_cutoff = 0.01 
                else: 
                    normalized_cutoff = cut_off_frequency / nyquist
            else: 
                # print("[CLIENT - FIR] Cannot compute normalized cutoff, Nyquist is zero. Skipping FIR.") # Too verbose
                return np.full_like(values, np.nan), np.array([]) 
        else:
            normalized_cutoff = cut_off_frequency / nyquist
        
        # print(f"[CLIENT - FIR] Final Normalized Cutoff: {normalized_cutoff:.4f}")

        numtaps = 51 
        if numtaps > len(values):
            numtaps = len(values) 
            if numtaps % 2 == 0 and numtaps > 0: numtaps -= 1 
            if numtaps < 1: 
                # print(f"[CLIENT - FIR] Not enough samples ({len(values)}) for FIR filter (numtaps={numtaps}). Skipping.") # Too verbose
                return np.full_like(values, np.nan), np.array([])

        fir_coefficients = firwin(numtaps=numtaps, cutoff=normalized_cutoff, window="hamming", pass_zero=True)
        
        # print(f"[CLIENT - FIR] FIR Coefficients (first 5): {fir_coefficients[:5]}... Sum: {np.sum(fir_coefficients):.4f}")

        if np.all(fir_coefficients == 0) or np.any(np.isnan(fir_coefficients)):
            print("[CLIENT - FIR] Warning: FIR coefficients are problematic (all zeros or NaN). Filter will have no effect or crash.")
            return np.full_like(values, np.nan), np.array([]) 


        if len(values) < len(fir_coefficients):
            # print(f"[CLIENT] Warning: Signal length ({len(values)}) too short for filter taps ({len(fir_coefficients)}). Skipping FIR filter.") # Too verbose
            return np.full_like(values, np.nan), fir_coefficients 

        filtered_values = lfilter(fir_coefficients, 1.0, values)
        # print(f"[CLIENT - FIR] Filtered values first 5: {filtered_values[:5]}") # DEBUG
        return filtered_values, fir_coefficients
    except Exception as e:
        print(f"[CLIENT] Error in fir_filter function: {e}")
        return np.full_like(values, np.nan), np.array([])  


def init_plot(label="ADC Data"):
    """Initializes the Matplotlib figure and axes for live plotting."""
    global current_fig, current_ax1, current_ax2, current_ax3, \
           current_raw_line, current_filtered_line, current_fft_line

    with plot_lock:
        plt.close('all') 
        
        current_fig, (current_ax1, current_ax2, current_ax3) = plt.subplots(3, 1, figsize=(10, 8), sharex=False)
        current_fig.subplots_adjust(hspace=0.5) 

        current_raw_line, = current_ax1.plot([], [], color='red', label="Raw Data")
        current_filtered_line, = current_ax2.plot([], [], color='black', label="FIR-Filtered Data")
        current_fft_line, = current_ax3.plot([], [], color='blue', label="FFT Spectrum")

        current_ax1.set_title(f"Raw ADC Data - {label}")
        current_ax1.set_xlabel("Sample Index")
        current_ax1.set_ylabel("Weight")
        current_ax1.grid(True)
        current_ax1.legend()

        current_ax2.set_title(f"FIR-Filtered ADC Data - {label}")
        current_ax2.set_xlabel("Sample Index")
        current_ax2.set_ylabel("Weight")
        current_ax2.grid(True)
        current_ax2.legend()

        current_ax3.set_title(f"FFT Spectrum - {label}")
        current_ax3.set_xlabel("Frequency (Hz)")
        current_ax3.set_ylabel("Magnitude")
        current_ax3.grid(True)
        current_ax3.legend()

        # Set initial limits more robustly using a default range, will be dynamically adjusted
        current_ax1.set_ylim(0, 700) 
        current_ax2.set_ylim(0, 700) 
        current_ax3.set_ylim(0, 1) # This is a placeholder, will be dynamic but helps during init
        current_ax1.set_xlim(0, current_raw_buffer.maxlen)
        current_ax2.set_xlim(0, current_filtered_buffer.maxlen)
        current_ax3.set_xlim(0, 100) 

        print(f"[CLIENT] Plot initialized for: {label}")


def update_live_plot(sampling_rate, label="ADC Data"):
    """
    Updates the Matplotlib plots using the global data buffers.
    This function is called repeatedly to simulate live data.
    """
    global current_fig, current_ax1, current_ax2, current_ax3
    global current_raw_line, current_filtered_line, current_fft_line

    if not plotting_active or current_fig is None:
        return

    with plot_lock:
        try:
            raw_data_to_plot = np.array(list(current_raw_buffer))
            filtered_data_to_plot = np.array(list(current_filtered_buffer))
            
            current_raw_line.set_data(np.arange(len(raw_data_to_plot)), raw_data_to_plot)
            current_filtered_line.set_data(np.arange(len(filtered_data_to_plot)), filtered_data_to_plot)

            if len(raw_data_to_plot) > current_raw_buffer.maxlen * 0.5: 
                current_ax1.set_xlim(len(raw_data_to_plot) - current_raw_buffer.maxlen, len(raw_data_to_plot))
            else:
                current_ax1.set_xlim(0, current_raw_buffer.maxlen) 

            current_ax2.set_xlim(current_ax1.get_xlim()) 

            # Robust Y-axis limits using nanmin/nanmax
            if raw_data_to_plot.size > 0 and not np.all(np.isnan(raw_data_to_plot)):
                min_y_raw, max_y_raw = np.nanmin(raw_data_to_plot) * 0.9, np.nanmax(raw_data_to_plot) * 1.1
                # Fallback if min_y_raw/max_y_raw become problematic (e.g., all same value or only nan)
                if not np.isfinite(min_y_raw) or not np.isfinite(max_y_raw) or (max_y_raw - min_y_raw) < 1e-9:
                    min_y_raw, max_y_raw = -0.1, 0.1 # Small default range
                current_ax1.set_ylim(min_y_raw, max_y_raw)
            else:
                current_ax1.set_ylim(-0.1, 0.1) # Default if no valid data

            if filtered_data_to_plot.size > 0 and not np.all(np.isnan(filtered_data_to_plot)):
                min_y_filtered, max_y_filtered = np.nanmin(filtered_data_to_plot) * 0.9, np.nanmax(filtered_data_to_plot) * 1.1
                if not np.isfinite(min_y_filtered) or not np.isfinite(max_y_filtered) or (max_y_filtered - min_y_filtered) < 1e-9:
                    min_y_filtered, max_y_filtered = -0.1, 0.1 # Small default range
                current_ax2.set_ylim(min_y_filtered, max_y_filtered)
            else:
                current_ax2.set_ylim(-0.1, 0.1) # Default if no valid data


            fft_window_size = min(256, len(list(dsp_raw_adc_buffer))) 
            if fft_window_size >= 2:
                fft_input_data = np.array(list(dsp_raw_adc_buffer))[-fft_window_size:] 
                
                processed_fft_input = remove_dc_offset_temp(fft_input_data) 
                
                fft_frequencies, fft_magnitude, _ = compute_fft(processed_fft_input, sampling_rate)
                
                # --- NEW DIAGNOSTIC PRINTS FOR FFT PLOTTING ---
                # print(f"[CLIENT - Plotting FFT] Freqs len: {len(fft_frequencies)}, Mags len: {len(fft_magnitude)}")
                # print(f"[CLIENT - Plotting FFT] Freqs (first 5): {fft_frequencies[:5]}")
                # print(f"[CLIENT - Plotting FFT] Mags (first 5): {fft_magnitude[:5]}")
                # print(f"[CLIENT - Plotting FFT] Max Freq: {np.nanmax(fft_frequencies) if len(fft_frequencies) > 0 else 'N/A'}, Max Mag: {np.nanmax(fft_magnitude) if len(fft_magnitude) > 0 else 'N/A'}")
                # --- END NEW DIAGNOSTIC PRINTS ---

                current_fft_line.set_data(fft_frequencies, fft_magnitude)

                if len(fft_frequencies) > 0 and np.nanmax(fft_frequencies) > 0:
                    current_ax3.set_xlim(0, np.nanmax(fft_frequencies) * 1.1)
                else:
                    current_ax3.set_xlim(0, sampling_rate / 2) # Default to Nyquist if no frequencies

                if len(fft_magnitude) > 0 and np.nanmax(fft_magnitude) > 0:
                    # Scale Y-axis more dynamically but exclude the extreme 0Hz if present
                    # Check if 0Hz is explicitly the first frequency bin and if there are other frequencies
                    if fft_frequencies.size > 0 and fft_frequencies[0] == 0 and fft_magnitude.size > 1: 
                        # Get magnitudes excluding the DC component at 0Hz
                        non_dc_magnitudes = fft_magnitude[1:]
                        if non_dc_magnitudes.size > 0 and np.nanmax(non_dc_magnitudes) > 0:
                            max_ac_magnitude = np.nanmax(non_dc_magnitudes)
                            current_ax3.set_ylim(0, max_ac_magnitude * 1.2)
                        else: # All AC magnitudes are zero or NaN, but DC is not
                            current_ax3.set_ylim(0, fft_magnitude[0] * 0.01 + 1e-9) # Show tiny fraction of DC or small default
                    else: # No 0Hz or only 0Hz or all values are small
                        current_ax3.set_ylim(0, np.nanmax(fft_magnitude) * 1.2)
                else:
                    current_ax3.set_ylim(0, 1) # Default if no valid FFT magnitude

            else:
                current_fft_line.set_data([], []) 
                current_ax3.set_xlim(0, sampling_rate / 2)
                current_ax3.set_ylim(0, 1)

            current_fig.canvas.draw()
            current_fig.canvas.flush_events()
            
        except Exception as e:
            print(f"[CLIENT] Error in update_live_plot: {e}")


def write_data_to_file(file_name, raw_weights_all, filtered_weights_all, fir_coefficients, fft_frequencies_last, fft_magnitude_last):
    """
    Writes all processed data to a text file for record-keeping.
    Collects full data set, not just the buffered part.
    """
    try:
        output_folder = "output_data"
        if not os.path.exists(output_folder):
            os.makedirs(output_folder)
            print(f"Created output folder: {output_folder}")

        filepath = os.path.join(output_folder, f"all_data_{file_name}.txt")
        with open(filepath, "w") as f:
            f.write(f"Raw Weights (total {len(raw_weights_all)} samples):\\n{list(raw_weights_all)}\\n\\n")
            f.write(f"Filtered Weights (total {len(filtered_weights_all)} samples):\\n{list(filtered_weights_all)}\\n\\n")
            
            f.write(f"FIR Coefficients (total {len(fir_coefficients)} samples):\\n{list(fir_coefficients) if len(fir_coefficients) > 0 else 'N/A'}\\n\\n")
            
            f.write(f"FFT Frequencies (last computed window, total {len(fft_frequencies_last)} samples):\\n{list(fft_frequencies_last)}\\n\\n")
            f.write(f"FFT Magnitudes (last computed window, total {len(fft_magnitude_last)} samples):\\n{list(fft_magnitude_last)}\\n\\n")
        print(f"[CLIENT] Successfully wrote all data for {file_name} to {filepath}")
    except Exception as e:
        print(f"[CLIENT] Error writing to file: {e}")


def process_and_plot_live_data(raw_adc_values_full_file, interval_ms, file_name):
    """
    Simulates live processing and plotting of data from a full received file.
    This runs on the main thread and introduces time delays for live effect.
    """
    global current_raw_buffer, current_filtered_buffer, dsp_raw_adc_buffer

    if len(raw_adc_values_full_file) == 0:
        print(f"[CLIENT] No raw ADC data to simulate live processing for {file_name}.")
        return

    print(f"[CLIENT] Simulating live processing for {file_name} (interval: {interval_ms}ms)...")
    
    all_raw_weights = []
    all_filtered_weights = []
    last_fir_coefficients = np.array([])
    last_fft_frequencies = np.array([])
    last_fft_magnitude = np.array([])

    sampling_rate = 1000.0 / interval_ms if interval_ms > 0 else 1.0
    # Number of filter taps
    num_taps = 51
    # Minimum samples needed for FIR and a reasonable FFT window
    min_dsp_samples = max(num_taps, 256) 


    init_plot(label=file_name)


    for i in range(len(raw_adc_values_full_file)):
        if not plotting_active: 
            print("[CLIENT] Live plotting simulation stopped by user/system.")
            break

        current_raw_adc = raw_adc_values_full_file[i]
        
        # Add raw ADC value to DSP buffer (will be DC-removed for processing)
        dsp_raw_adc_buffer.append(current_raw_adc) 

        # Normalize current raw ADC value to weight and add to raw display buffer
        current_raw_weight = normalize_to_weights([current_raw_adc])[0]
        current_raw_buffer.append(current_raw_weight)
        all_raw_weights.append(current_raw_weight)

        
        filtered_weight_to_plot = np.nan # Initialize as NaN for plotting gap
        
        # Only perform DSP if we have enough data in the buffer for reliable calculations
        if len(dsp_raw_adc_buffer) >= min_dsp_samples: 
            current_dsp_raw_values = np.array(list(dsp_raw_adc_buffer))

            processed_window_dc_removed = remove_dc_offset_temp(current_dsp_raw_values)

            fft_frequencies, fft_magnitude, cut_off_frequency = compute_fft(processed_window_dc_removed, sampling_rate)
            last_fft_frequencies = fft_frequencies
            last_fft_magnitude = fft_magnitude
            
            filtered_segment_values_dc_removed, fir_coefficients = fir_filter(processed_window_dc_removed, cut_off_frequency, sampling_rate)
            last_fir_coefficients = fir_coefficients
            
            if len(filtered_segment_values_dc_removed) > 0:
                filtered_point_dc_removed = filtered_segment_values_dc_removed[-1]
                
                original_mean_of_window = np.mean(current_dsp_raw_values)
                re_offset_filtered_value = filtered_point_dc_removed + original_mean_of_window
                filtered_weight_to_plot = normalize_to_weights([re_offset_filtered_value])[0]
            else:
                if processed_window_dc_removed.size > 0:
                    original_mean_of_window = np.mean(current_dsp_raw_values)
                    re_offset_value = processed_window_dc_removed[-1] + original_mean_of_window
                    filtered_weight_to_plot = normalize_to_weights([re_offset_value])[0]
                else:
                    filtered_weight_to_plot = current_raw_weight 


        if len(dsp_raw_adc_buffer) >= num_taps:
            current_filtered_buffer.append(filtered_weight_to_plot)
        else:
            current_filtered_buffer.append(np.nan) 

        all_filtered_weights.append(filtered_weight_to_plot if len(dsp_raw_adc_buffer) >= num_taps else np.nan)


        update_live_plot(sampling_rate, label=file_name)
        
        plt.pause(interval_ms / 1000.0) 

    print(f"[CLIENT] Finished simulating live processing for {file_name}. Saving full data.")
    write_data_to_file(file_name, np.array(all_raw_weights), np.array(all_filtered_weights), last_fir_coefficients, last_fft_frequencies, last_fft_magnitude)


def recvall(sock, n):
    """Helper function to receive N bytes reliably or return None if EOF is hit."""
    data = b''
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data += packet
    return data


def receive_and_queue_data_loop(data_queue):
    """
    Connects to the server, receives data using the length-prefixing protocol,
    and puts parsed data (full file) into a queue for the main thread to process.
    This function runs in a separate background thread.
    """
    global network_thread_running 
    network_thread_running = True 

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((SERVER_IP, SERVER_PORT))
        print("[CLIENT] Connected to server.")
        
        interval_ms = 20 
        mode = "interval"

        config_len_bytes = recvall(s, CONFIG_LENGTH_BYTES)
        if not config_len_bytes:
            print("[CLIENT] Server disconnected while receiving config length (config).")
            return
        config_len = int.from_bytes(config_len_bytes, 'big')
        
        config_data_bytes = recvall(s, config_len)
        if not config_data_bytes:
            print("[CLIENT] Server disconnected while receiving config data (config).")
            return
        
        config_data = config_data_bytes.decode('utf-8').strip()
        print(f"[CLIENT] Received config: {config_data}")
        
        for line in config_data.split('\\n'):
            if "INTERVAL:" in line:
                try:
                    interval_ms = int(line.split(":")[1])
                    print(f"[CLIENT] Set interval: {interval_ms} ms")
                except ValueError:
                    print(f"[CLIENT] Warning: Could not parse interval from: {line}")
            elif "MODE:" in line:
                mode = line.split(":")[1]
                print(f"[CLIENT] Set mode: {mode}")

        while True:
            filename_len_bytes = recvall(s, FILENAME_LENGTH_BYTES)
            if not filename_len_bytes:
                print("[CLIENT] Server disconnected or no more files to receive (filename length).")
                break 
            filename_length = int.from_bytes(filename_len_bytes, 'big')

            filename_bytes = recvall(s, filename_length)
            if not filename_bytes:
                print("[CLIENT] Server disconnected while receiving filename.")
                break
            file_name = filename_bytes.decode('utf-8')
            print(f"[CLIENT] Received file name: {file_name}")

            file_content_len_bytes = recvall(s, FILE_CONTENT_LENGTH_BYTES)
            if not file_content_len_bytes:
                print("[CLIENT] Server disconnected while receiving file content length.")
                break
            file_content_length = int.from_bytes(file_content_len_bytes, 'big')

            if file_name == "END_OF_TRANSMISSION":
                print("[CLIENT] Received END_OF_TRANSMISSION signal from server. Stopping file reception.")
                break 
            elif file_name.startswith("NO_FILE_FOUND:"):
                print(f"[CLIENT] Server message: {file_name}. Stopping file reception.")
                break 
            elif file_name == "NO_FILE_SELECTED":
                print(f"[CLIENT] Server message: {file_name}. Stopping file reception.")
                break 
            elif file_name == "NO_FILES_IN_FOLDER": 
                print(f"[CLIENT] Server message: No .txt files found in server's folder. Stopping reception.")
                break

            print(f"[CLIENT] Expecting file content of length: {file_content_length} bytes for {file_name}")

            file_content_data = recvall(s, file_content_length)
            if not file_content_data:
                print(f"[CLIENT] Server disconnected while receiving file content for {file_name}.")
                break

            print(f"[CLIENT] Received file content. Actual Length: {len(file_content_data)} bytes.")

            lines = file_content_data.decode(errors='ignore').splitlines()
            raw_adc_values = []
            for line in lines:
                if 'ADC:' in line:
                    try:
                        adc = int(line.strip().split("ADC:")[-1])
                        raw_adc_values.append(adc)
                    except ValueError:
                        print(f"[CLIENT] Warning: Invalid ADC value in line: {line}. Skipping.")
                        pass
            
            if raw_adc_values:
                data_queue.put((raw_adc_values, interval_ms, file_name))
                print(f"[CLIENT] Put full file '{file_name}' into queue for live simulation.")
            else:
                print(f"[CLIENT] No valid ADC values found in file {file_name}. Not adding to queue.")
                
    except ConnectionRefusedError:
        print("[CLIENT] Error: Connection refused. Is the server running and listening on the correct IP/port?")
    except ConnectionResetError:
        print("[CLIENT] Error: Connection with server was reset unexpectedly.")
    except ConnectionAbortedError:
        print("[CLIENT] Error: Connection with server was aborted unexpectedly.")
    except Exception as e:
        print(f"[CLIENT] An unexpected error occurred in receive_and_queue_data_loop: {e}")
    finally:
        s.close()
        print("[CLIENT] Network connection closed.")
        network_thread_running = False 


if __name__ == "__main__":
    plt.ion() 
    plotting_active = True 

    data_queue = queue.Queue()

    data_thread = threading.Thread(target=receive_and_queue_data_loop, args=(data_queue,), daemon=True)
    data_thread.start()

    try:
        while plotting_active or not data_queue.empty() or data_thread.is_alive() or network_thread_running:
            try:
                raw_adc_values_full_file, interval_ms, file_name = data_queue.get(timeout=0.1) 
                print(f"[CLIENT MAIN] Pulled full file '{file_name}' from queue. Starting live simulation...")
                
                current_raw_buffer.clear()
                current_filtered_buffer.clear()
                dsp_raw_adc_buffer.clear() 

                process_and_plot_live_data(raw_adc_values_full_file, interval_ms, file_name)
                
            except queue.Empty:
                plt.pause(0.01) 
            except Exception as e:
                print(f"[CLIENT MAIN] Error in main loop while getting data from queue: {e}")
                plotting_active = False 
                break
            
            if not plotting_active and data_queue.empty() and not data_thread.is_alive() and not network_thread_running:
                print("[CLIENT MAIN] All files processed and network thread finished. Exiting plotting loop.")
                break

    except Exception as e:
        print(f"[CLIENT] An unexpected error occurred in the main Matplotlib loop: {e}")
    finally:
        print("[CLIENT] Exiting client application.")
        plotting_active = False 
        plt.ioff() 
        try:
            if current_fig and plt.fignum_exists(current_fig.number):
                print("[CLIENT] Displaying final plot. Close the window to exit the application.")
                plt.show(block=True) 
            else:
                print("[CLIENT] No plot window to display or it was already closed.")
        except Exception as e:
            print(f"[CLIENT] Error showing final plot: {e}")