# File: standalone_fir_analyzer.py
# FINAL version: A standalone application with a real-time file loader.

import matplotlib.pyplot as plt
from matplotlib.widgets import RadioButtons, Button
import numpy as np
import time
from scipy.signal import firwin
import os
from collections import deque
import tkinter as tk
from tkinter import filedialog
import threading

try:
    import cmsisdsp as dsp
    version_string = f"v{dsp.__version__}" if hasattr(dsp, '__version__') else "(version unknown)"
    print(f"[APP] CMSIS-DSP library {version_string} loaded successfully.")
except ImportError:
    print("[APP] ERROR: CMSIS-DSP library not found. Please install it using 'pip install cmsisdsp'")
    exit()

# --- Global state variables ---
USER_SELECTED_INTERVAL_MS = 20  # Default plotting speed
simulation_running = False # Flag to prevent overlapping simulations

# Configuration
FIR_CUTOFF_HZ = 10.0

# Calibration constants
ZERO_CAL = 0.01823035255075
SCALE_CAL = 0.00000451794631

# Global variables for plotting
current_raw_buffer = deque(maxlen=500)
current_filtered_buffer = deque(maxlen=500)
dsp_raw_adc_buffer = deque(maxlen=500)
current_fig, current_axes = None, None
current_raw_line, current_filtered_line = None, None
plot_lock = threading.Lock() # Retained for thread-safe matplotlib calls
# Widgets need to be global to prevent garbage collection
speed_radio_buttons = None
load_button = None


def calculate_amplitudes_and_dc(raw_adc_values):
    """Calculates amplitudes and the STABLE DC offset for the entire signal."""
    print("[APP] Calculating signal amplitudes and stable DC offset...")
    if not raw_adc_values:
        return None, None, 0

    try:
        # Assuming a fixed sampling rate for analysis based on typical data
        sampling_rate = 50.0 
        
        signal_as_f32 = np.array(raw_adc_values, dtype=np.float32)
        stable_dc_offset = np.mean(signal_as_f32)
        print(f"[APP] Stable DC offset calculated: {stable_dc_offset:.2f}")

        raw_weights = normalize_to_weights(raw_adc_values)
        raw_amplitude = np.nanmax(raw_weights) - np.nanmin(raw_weights)
        print(f"[APP] Raw data amplitude (peak-to-peak): {raw_amplitude:.2f}")
        
        dc_removed_signal = signal_as_f32 - stable_dc_offset
        filtered_ac_signal, _ = fir_filter(dc_removed_signal, FIR_CUTOFF_HZ, sampling_rate)

        if filtered_ac_signal.size > 0 and not np.all(np.isnan(filtered_ac_signal)):
            reconstructed_filtered_signal = filtered_ac_signal + stable_dc_offset
            filtered_weights = normalize_to_weights(reconstructed_filtered_signal)
            filtered_amplitude = np.nanmax(filtered_weights) - np.nanmin(filtered_weights)
            print(f"[APP] Filtered data amplitude (peak-to-peak): {filtered_amplitude:.2f}")
        else:
            filtered_amplitude = None
            print("[APP] Could not generate filtered signal to calculate amplitude.")
            
        return raw_amplitude, filtered_amplitude, stable_dc_offset
        
    except Exception as e:
        print(f"[APP] Error during amplitude calculation: {e}")
        return None, None, 0

def save_amplitude_results(original_file_name, raw_amplitude, filtered_amplitude):
    """Saves the amplitude calculation results to a unique file."""
    output_directory = "analysis_results"
    
    try:
        if not os.path.exists(output_directory):
            os.makedirs(output_directory)
            print(f"[APP] Created output directory: {output_directory}")

        # Extract just the filename from the path
        base_name = os.path.basename(original_file_name)
        output_filename = f"analysis_{base_name}"
        output_filepath = os.path.join(output_directory, output_filename)
        
        with open(output_filepath, "w") as f:
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            raw_amp_str = f"{raw_amplitude:.4f}" if raw_amplitude is not None else "N/A"
            filt_amp_str = f"{filtered_amplitude:.4f}" if filtered_amplitude is not None else "N/A"
            f.write(f"Analysis for file: {base_name}\n")
            f.write(f"Analysis timestamp: {timestamp}\n")
            f.write("-" * 30 + "\n")
            f.write(f"Raw Data Peak-to-Peak Amplitude: {raw_amp_str}\n")
            f.write(f"Filtered Data Peak-to-Peak Amplitude: {filt_amp_str}\n")
            
        print(f"[APP] Amplitude results saved to: {output_filepath}")
        
    except Exception as e:
        print(f"[APP] Error saving amplitude to file: {e}")


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


def remove_dc_offset_temp(values):
    """Remove DC offset and ensure data is float32 for DSP functions."""
    try:
        values_array = np.array(values, dtype=np.float32)
        if values_array.size == 0:
            return np.array([], dtype=np.float32)
        return values_array - np.nanmean(values_array)
    except Exception as e:
        print(f"[APP] Error in remove_dc_offset_temp: {e}")
        return np.full_like(np.array(values, dtype=np.float32), np.nan)


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
        print(f"[APP] Error in fir_filter (CMSIS-DSP): {e}")
        return np.full_like(values, np.nan), np.array([], dtype=np.float32)


def on_load_file(event):
    """Callback function for the 'Load File' button."""
    global simulation_running
    if simulation_running:
        print("[APP] A simulation is already running. Please wait for it to finish.")
        return

    # Hide the root tkinter window
    root = tk.Tk()
    root.withdraw()
    
    # Open file dialog
    filepath = filedialog.askopenfilename(
        title="Select a Data File",
        filetypes=(("Text files", "*.txt"), ("All files", "*.*"))
    )
    if not filepath:
        print("[APP] No file selected.")
        return

    simulation_running = True
    print(f"[APP] Loading data from: {filepath}")
    
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        raw_adc_values = []
        for line in lines:
            if 'ADC:' in line:
                try:
                    adc_val = int(line.strip().split("ADC:")[-1])
                    raw_adc_values.append(adc_val)
                except (ValueError, IndexError):
                    pass
        
        if raw_adc_values:
            # Clear previous data and start the new simulation
            current_raw_buffer.clear()
            current_filtered_buffer.clear()
            dsp_raw_adc_buffer.clear()
            process_and_plot_live_data(raw_adc_values, filepath)
        else:
            print("[APP] No valid ADC data found in the selected file.")
    except Exception as e:
        print(f"[APP] Error reading or processing file: {e}")
    finally:
        simulation_running = False


def init_plot():
    """Initializes the Matplotlib figure, axes, and interactive widgets."""
    global current_fig, current_axes, current_raw_line, current_filtered_line, speed_radio_buttons, load_button
    with plot_lock:
        plt.close('all')
        current_fig, current_axes = plt.subplots(2, 1, figsize=(10, 6))
        plt.subplots_adjust(left=0.25, bottom=0.2)

        current_raw_line, = current_axes[0].plot([], [], 'r-', label="Raw Data")
        current_filtered_line, = current_axes[1].plot([], [], 'k-', label=f"FIR-Filtered @ {FIR_CUTOFF_HZ}Hz (CMSIS-DSP)")
        
        current_axes[0].set_title("Load a file to begin analysis")
        current_axes[0].set_ylabel("Weight")
        current_axes[0].grid(True)
        current_axes[0].legend(loc='upper right')
        
        current_axes[1].set_title("Filtered Data")
        current_axes[1].set_xlabel("Sample Index")
        current_axes[1].set_ylabel("Weight")
        current_axes[1].grid(True)
        current_axes[1].legend(loc='upper right')
        
        # --- Speed Control Widget ---
        speed_widget_ax = plt.axes([0.05, 0.5, 0.15, 0.25])
        speed_options = ["1ms", "2ms", "5ms", "10ms", "20ms", "50ms", "100ms"]
        speed_radio_buttons = RadioButtons(speed_widget_ax, speed_options, active=4)
        def on_speed_change(label):
            global USER_SELECTED_INTERVAL_MS
            USER_SELECTED_INTERVAL_MS = int(label.replace('ms', ''))
            print(f"[APP] Real-time speed changed to: {USER_SELECTED_INTERVAL_MS}ms interval.")
        speed_radio_buttons.on_clicked(on_speed_change)

        # --- Load File Widget ---
        load_widget_ax = plt.axes([0.05, 0.3, 0.15, 0.1])
        load_button = Button(load_widget_ax, 'Load Data File')
        load_button.on_clicked(on_load_file)


def update_live_plot():
    """Updates the Matplotlib plots using global data buffers."""
    if current_fig is None: return
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
                 print(f"[APP] Error during plot update: {e}")


def process_and_plot_live_data(raw_adc_values, file_name):
    """Calculates amplitudes and DC offset, saves them, then simulates live plotting."""
    if not raw_adc_values:
        print(f"[APP] No data to process for {file_name}.")
        return

    # Use a fixed sampling rate for one-time analysis
    sampling_rate = 50.0 
    raw_amp, filtered_amp, stable_dc_offset = calculate_amplitudes_and_dc(raw_adc_values)
    save_amplitude_results(file_name, raw_amp, filtered_amp)

    print(f"[APP] Starting live simulation for {file_name}...")
    
    # Update plot titles with the new filename
    base_name = os.path.basename(file_name)
    current_axes[0].set_title(f"Raw ADC Data - {base_name}")
    current_axes[1].set_title(f"FIR-Filtered Data - {base_name}")

    num_taps = 51
    min_dsp_samples = num_taps
    
    for adc_value in raw_adc_values:
        current_interval_ms = USER_SELECTED_INTERVAL_MS
        
        dsp_raw_adc_buffer.append(adc_value)
        raw_weight = normalize_to_weights([adc_value])[0]
        current_raw_buffer.append(raw_weight)
        
        filtered_weight = np.nan
        if len(dsp_raw_adc_buffer) >= min_dsp_samples:
            dsp_window = np.array(list(dsp_raw_adc_buffer)[-min_dsp_samples:], dtype=np.float32)
            processed_window = dsp_window - stable_dc_offset
            filtered_ac, _ = fir_filter(processed_window, FIR_CUTOFF_HZ, sampling_rate)
            
            if filtered_ac.size > 0 and not np.all(np.isnan(filtered_ac)):
                reconstructed_signal = filtered_ac[-1] + stable_dc_offset
                filtered_weight = normalize_to_weights([reconstructed_signal])[0]
        
        current_filtered_buffer.append(filtered_weight)
        update_live_plot()
        plt.pause(current_interval_ms / 1000.0)

    print(f"[APP] Finished simulation for {file_name}.")


if __name__ == "__main__":
    # The main block now just initializes the plot and shows it.
    # All action is driven by the user clicking the widgets.
    init_plot()
    print("[APP] Standalone Analyzer ready. Please use the 'Load Data File' button to begin.")
    plt.show() # This shows the plot and blocks until it is closed.
    print("[APP] Application exited.")
