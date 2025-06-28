import matplotlib.pyplot as plt
from matplotlib.widgets import RadioButtons
from matplotlib.animation import FuncAnimation
import numpy as np
import time
from scipy.signal import firwin
import os
from collections import deque
import tkinter as tk
from tkinter import filedialog
import threading
import queue

try:
    import cmsisdsp as dsp
    version_string = f"v{dsp.__version__}" if hasattr(dsp, '__version__') else "(version unknown)"
    print(f"[APP] CMSIS-DSP library {version_string} loaded successfully.")
except ImportError:
    print(f"[APP] ERROR: CMSIS-DSP library not found. Please install it using 'pip install cmsisdsp'")
    exit()

# --- Global state variables ---
USER_SELECTED_INTERVAL_MS = 20
selected_directory = ""
g_animation = None
g_all_raw_weights = np.array([])
g_all_filtered_weights = np.array([])
# --- RECTIFIED: Removed g_simulation_running flag in favor of managing the g_animation object directly ---

PLOT_WINDOW_SIZE = 500

# Configuration
FIR_CUTOFF_HZ = 10.0

# Calibration constants
ZERO_CAL = 0.01823035255075
SCALE_CAL = 0.00000451794631

# Global variables for plotting
current_fig, current_axes = None, None
current_raw_line, current_filtered_line = None, None
speed_radio_buttons = None
file_radio_buttons = None


def calculate_and_process_data(raw_adc_values, file_name):
    """
    Performs all one-time analysis and processing for a given file.
    Returns the complete, plottable data arrays.
    """
    print(f"[APP] Processing file: {file_name}")
    if not raw_adc_values:
        return None, None

    try:
        sampling_rate = 50.0 
        signal_as_f32 = np.array(raw_adc_values, dtype=np.float32)
        stable_dc_offset = np.mean(signal_as_f32)
        
        raw_weights = normalize_to_weights(raw_adc_values)
        raw_amplitude = np.nanmax(raw_weights) - np.nanmin(raw_weights)
        
        dc_removed_signal = signal_as_f32 - stable_dc_offset
        filtered_ac_signal, _ = fir_filter(dc_removed_signal, FIR_CUTOFF_HZ, sampling_rate)

        filtered_weights = np.full_like(raw_weights, np.nan)
        if filtered_ac_signal.size > 0 and not np.all(np.isnan(filtered_ac_signal)):
            reconstructed_filtered_signal = filtered_ac_signal + stable_dc_offset
            filtered_weights = normalize_to_weights(reconstructed_filtered_signal)
            filtered_amplitude = np.nanmax(filtered_weights) - np.nanmin(filtered_weights)
        else:
            filtered_amplitude = None
            
        save_amplitude_results(file_name, raw_amplitude, filtered_amplitude)
        return raw_weights, filtered_weights
        
    except Exception as e:
        print(f"[APP] Error during data processing: {e}")
        return None, None


def save_amplitude_results(original_file_name, raw_amplitude, filtered_amplitude):
    """Saves the amplitude calculation results to a unique file."""
    output_directory = "analysis_results"
    try:
        if not os.path.exists(output_directory):
            os.makedirs(output_directory)
        base_name = os.path.basename(original_file_name)
        output_filename = f"analysis_{base_name}"
        output_filepath = os.path.join(output_directory, output_filename)
        with open(output_filepath, "w") as f:
            raw_amp_str = f"{raw_amplitude:.4f}" if raw_amplitude is not None else "N/A"
            filt_amp_str = f"{filtered_amplitude:.4f}" if filtered_amplitude is not None else "N/A"
            f.write(f"Analysis for file: {base_name}\n")
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


def on_file_change(label, data_queue):
    """Callback for file selection. Stops previous animation and starts a new processing thread."""
    global selected_directory, g_animation
    
    # --- RECTIFIED: Stop any existing animation immediately on click ---
    if g_animation and g_animation.event_source:
        g_animation.event_source.stop()
        g_animation = None
        print("[APP] Previous animation stopped.")
    
    filepath = os.path.join(selected_directory, label)
    print(f"[APP] User selected new file: {filepath}")
    
    # Start a background thread to load and process the file
    thread = threading.Thread(target=load_and_process_file_worker, args=(filepath, data_queue))
    thread.daemon = True
    thread.start()


def load_and_process_file_worker(filepath, data_queue):
    """
    Worker thread function: Reads a file, processes it, and puts the result in a queue.
    """
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        raw_adc_values = [int(line.strip().split("ADC:")[-1]) for line in lines if 'ADC:' in line]
        
        if raw_adc_values:
            raw_weights, filtered_weights = calculate_and_process_data(raw_adc_values, filepath)
            if raw_weights is not None:
                # Put the fully processed data into the queue for the main thread
                data_queue.put((raw_weights, filtered_weights, filepath))
            else:
                data_queue.put(None) # Signal error
        else:
            print("[APP] No valid ADC data found in the selected file.")
            data_queue.put(None)
    except Exception as e:
        print(f"[APP] Error reading or processing file: {e}")
        data_queue.put(None)


def init_plot(file_list, data_queue):
    """Initializes the Matplotlib figure and all interactive widgets."""
    global current_fig, current_axes, current_raw_line, current_filtered_line, speed_radio_buttons, file_radio_buttons

    plt.close('all')
    current_fig, current_axes = plt.subplots(2, 1, figsize=(12, 7))
    plt.subplots_adjust(left=0.35, bottom=0.1)

    # Initial empty plot lines, animated=True is key for FuncAnimation
    current_raw_line, = current_axes[0].plot([], [], 'r-', label="Raw Data", animated=True)
    current_filtered_line, = current_axes[1].plot([], [], 'k-', label=f"FIR-Filtered @ {FIR_CUTOFF_HZ}Hz (CMSIS-DSP)", animated=True)
    
    for ax in current_axes:
        ax.grid(True)
        ax.legend(loc='upper right')
    
    current_axes[0].set_title("Select a file from the list to begin analysis")
    current_axes[0].set_ylabel("Weight")
    current_axes[1].set_title("Filtered Data")
    current_axes[1].set_xlabel("Sample Index")
    current_axes[1].set_ylabel("Weight")
    
    # Speed Control Widget
    current_fig.text(0.06, 0.9, "Plotting Speed", fontsize=10, weight='bold')
    speed_widget_ax = plt.axes([0.06, 0.65, 0.2, 0.25])
    speed_options = ["1ms", "2ms", "5ms", "10ms", "20ms", "50ms", "100ms"]
    speed_radio_buttons = RadioButtons(speed_widget_ax, speed_options, active=4)
    def on_speed_change(label):
        global USER_SELECTED_INTERVAL_MS
        USER_SELECTED_INTERVAL_MS = int(label.replace('ms', ''))
        # If an animation is running, update its interval in real-time
        if g_animation and g_animation.event_source:
            g_animation.event_source.interval = USER_SELECTED_INTERVAL_MS
        print(f"[APP] Real-time speed changed to: {USER_SELECTED_INTERVAL_MS}ms interval.")
    speed_radio_buttons.on_clicked(on_speed_change)

    # File Selection Widget
    current_fig.text(0.06, 0.5, "Data Files", fontsize=10, weight='bold')
    file_widget_ax = plt.axes([0.06, 0.1, 0.2, 0.4])
    display_names = [name[:25] + '...' if len(name) > 25 else name for name in file_list]
    file_radio_buttons = RadioButtons(file_widget_ax, display_names)
    
    def file_change_handler(label):
        original_label = file_list[display_names.index(label)]
        on_file_change(original_label, data_queue)
        
    file_radio_buttons.on_clicked(file_change_handler)
    return current_fig


def update_animation_frame(frame):
    """Function called by FuncAnimation for each frame."""
    # Update the lines with data up to the current frame
    current_raw_line.set_data(np.arange(frame + 1), g_all_raw_weights[:frame + 1])
    current_filtered_line.set_data(np.arange(frame + 1), g_all_filtered_weights[:frame + 1])
    
    # Implement scrolling x-axis
    if frame < PLOT_WINDOW_SIZE:
        current_axes[0].set_xlim(0, PLOT_WINDOW_SIZE)
        current_axes[1].set_xlim(0, PLOT_WINDOW_SIZE)
    else:
        current_axes[0].set_xlim(frame - PLOT_WINDOW_SIZE, frame)
        current_axes[1].set_xlim(frame - PLOT_WINDOW_SIZE, frame)

    return current_raw_line, current_filtered_line


def start_new_animation(all_raw, all_filtered, file_name):
    """Sets up and starts a new animation in the main thread."""
    global g_all_raw_weights, g_all_filtered_weights, g_animation, g_simulation_running

    g_all_raw_weights = all_raw
    g_all_filtered_weights = all_filtered
    
    base_name = os.path.basename(file_name)
    current_axes[0].set_title(f"Raw ADC Data - {base_name}")
    current_axes[1].set_title(f"FIR-Filtered Data - {base_name}")
    
    # Reset plot limits based on the new data
    display_width = min(len(g_all_raw_weights), PLOT_WINDOW_SIZE)
    for ax in current_axes:
        # Clear previous data and reset limits
        ax.set_xlim(0, display_width)

    if g_all_raw_weights.size > 0 and np.any(np.isfinite(g_all_raw_weights)):
        min_y, max_y = np.nanmin(g_all_raw_weights), np.nanmax(g_all_raw_weights)
        padding = (max_y - min_y) * 0.1 or 1
        current_axes[0].set_ylim(min_y - padding, max_y + padding)
        
    if g_all_filtered_weights.size > 0 and np.any(np.isfinite(g_all_filtered_weights)):
        min_y, max_y = np.nanmin(g_all_filtered_weights), np.nanmax(g_all_filtered_weights)
        padding = (max_y - min_y) * 0.1 or 1
        current_axes[1].set_ylim(min_y - padding, max_y + padding)

    # Create and start the new animation
    g_animation = FuncAnimation(
        current_fig, 
        update_animation_frame, 
        frames=len(g_all_raw_weights),
        interval=USER_SELECTED_INTERVAL_MS, 
        blit=True, 
        repeat=False
    )
    current_fig.canvas.draw()
    g_simulation_running = True # Signal that an animation is now active


if __name__ == "__main__":
    root = tk.Tk()
    root.withdraw()

    print("[APP] Please select the directory containing your data files.")
    selected_directory = filedialog.askdirectory(title="Select Data File Directory")

    if not selected_directory:
        print("[APP] No directory selected. Exiting.")
        exit()
        
    print(f"[APP] Scanning for .txt files in: {selected_directory}")
    
    try:
        file_list = sorted([f for f in os.listdir(selected_directory) if f.endswith('.txt')])
        if not file_list:
            print("[APP] No .txt files found in the selected directory. Exiting.")
            exit()
    except Exception as e:
        print(f"[APP] Error scanning directory: {e}")
        exit()

    data_for_plotting_queue = queue.Queue()
    fig = init_plot(file_list, data_for_plotting_queue)
    
    # --- RECTIFIED: Main application loop using a periodic timer ---
    def check_queue():
        """Function to be called periodically to check for new data to plot."""
        global g_simulation_running
        try:
            plot_data = data_for_plotting_queue.get_nowait()
            
            if plot_data:
                all_raw, all_filtered, file_name = plot_data
                start_new_animation(all_raw, all_filtered, file_name)
            else: # Received a None, indicating processing failed
                g_simulation_running = False
                print("[APP] Ready to load another file.")
        except queue.Empty:
            # This is normal, just means no new file has been processed
            pass
        
    # Use a timer that is part of the Matplotlib canvas for safe, cross-platform GUI updates
    timer = fig.canvas.new_timer(interval=100) # Check queue every 100ms
    timer.add_callback(check_queue)
    timer.start()

    print("[APP] Standalone Analyzer ready. Please select a file from the list to begin.")
    
    plt.show()

    print("[APP] Application exited.")
