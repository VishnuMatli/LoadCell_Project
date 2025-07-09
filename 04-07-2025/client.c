#include <stdio.h>       // For printf, fprintf, fopen, fclose
#include <stdlib.h>      // For malloc, free, exit, atoi
#include <string.h>      // For strcmp, strcpy, strlen, strtok, strstr
#include <stdint.h>      // For uint32_t, uint64_t
#include <math.h>        // For sqrt, fabs, NAN, isnan
#include <direct.h>      // For _mkdir on Windows

// === Windows Specific Includes ===
#include <winsock2.h>    // For SOCKET, WSAStartup, WSACleanup, etc.
#include <ws2tcpip.h>    // For inet_pton
#include <windows.h>     // For CreateThread, Sleep, CRITICAL_SECTION, CONDITION_VARIABLE, etc.

// Link with Ws2_32.lib (add to compiler command)
#pragma comment(lib, "Ws2_32.lib") 

// === GTK Specific Includes ===
#include <gtk/gtk.h>     // For GTK widgets and functions
#include <cairo.h>       // For drawing in GtkDrawingArea

// === Configuration ===
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999
#define BUFFER_SIZE 4096

#define FILENAME_LENGTH_BYTES 4
#define FILE_CONTENT_LENGTH_BYTES 8
#define CONFIG_LENGTH_BYTES 4

// Calibration constants for converting ADC values to meaningful weights
#define ZERO_CAL 0.01823035255075
#define SCALE_CAL 0.00000451794631
#define ADC_MAX_VAL 2147483648.0 // 0x80000000 as a double

// Buffer sizes for live plot and DSP operations (equivalent to Python's deque maxlen)
#define PLOT_BUFFER_SIZE 500
#define DSP_BUFFER_SIZE 500 // Should be >= max(FIR_NUM_TAPS, FFT_WINDOW_SIZE)
#define FFT_WINDOW_SIZE 256
#define FIR_NUM_TAPS 51

// === Global Data Structures and Synchronization ===

// Circular Buffer implementation (similar to Python's collections.deque)
typedef struct {
    double* data;
    int head; // Index of the oldest element
    int tail; // Index where the next element will be inserted
    int count; // Current number of elements in the buffer
    int max_size; // Maximum capacity of the buffer
} CircularBuffer;

CircularBuffer current_raw_buffer;       // Buffer for raw data points (normalized to weights) for live plot
CircularBuffer current_filtered_buffer;  // Buffer for filtered data points for live plot (DC-removed for plotting)
CircularBuffer dsp_raw_adc_buffer;       // Buffer to hold raw ADC values for DSP operations (FFT/FIR input)

// Windows synchronization primitives
CRITICAL_SECTION plot_lock;        // Protects current_raw_buffer, current_filtered_buffer, dsp_raw_adc_buffer during access
CRITICAL_SECTION queue_lock;       // Protects the data_queue
CONDITION_VARIABLE queue_cond;     // Used to signal the main thread when data is available in data_queue

// Flags to control thread execution
volatile int plotting_active = 0;      // Controls if the plotting and processing loop should continue
volatile int network_thread_running = 0; // Indicates if the network thread is active

// Data structure to hold a full file's data received from the network
typedef struct {
    double* raw_adc_values;  // Dynamically allocated array of raw ADC values for one file
    int num_samples;         // Number of samples in this file
    int interval_ms;         // Sampling interval for this file (determines sampling rate)
    char file_name[256];     // Name of the file (for display/saving)
} QueuedData;

// Simple array-based circular queue for passing file data from network thread to main thread
QueuedData* data_queue[10]; // Max 10 files in queue
int queue_head = 0;
int queue_tail = 0;
int queue_count = 0;

// Global state for managing the processing of a single file in the GUI's main loop
typedef struct {
    double* current_file_raw_adc_values; // The full raw ADC data for the file currently being processed
    int current_file_num_samples;       // Total samples in the current file
    int current_file_interval_ms;       // Sampling interval for the current file
    char current_file_name[256];        // Name of the current file
    int current_file_index;             // Index of the next sample to process in current_file_raw_adc_values
    int is_processing_file;             // Flag: 1 if a file is currently being processed, 0 otherwise
    
    // Data collected for saving to file (these are dynamically growing arrays)
    double* all_raw_weights_to_save;
    int all_raw_weights_len_to_save;
    double* all_filtered_weights_to_save; // This will store DC-retained filtered data if needed, or DC-removed
    int all_filtered_weights_len_to_save;
    
    // Last computed DSP results (these are updated for the plot and saved at end of file)
    double* last_fir_coefficients_to_save;
    int last_fir_coefficients_len_to_save;
    double* last_fft_frequencies_to_save;
    int last_fft_frequencies_len_to_save;
    double* last_fft_magnitude_to_save;
    int last_fft_magnitude_len_to_save;
} FileProcessingState;

FileProcessingState g_file_state = {0}; // Initialize global state to zeros/NULLs

// GTK Widget pointers for access in callbacks
GtkWidget *main_window = NULL;
GtkWidget *raw_plot_area = NULL;       // Dedicated drawing area for raw data
GtkWidget *filtered_plot_area = NULL;  // Dedicated drawing area for filtered data
GtkWidget *fft_plot_area = NULL;       // Dedicated drawing area for FFT data
GtkWidget *label_status = NULL;        // For displaying messages

// Global variable to hold the ID of the GSource (timeout) for data processing
guint data_processing_source_id = 0; 

// Custom 64-bit host-to-network byte swap (needed for FILE_CONTENT_LENGTH_BYTES)
uint64_t ntohll_custom(uint64_t val) {
    return (((uint64_t)ntohl((uint32_t)(val & 0xFFFFFFFF))) << 32) | htonl((uint32_t)(val >> 32));
}

// === Function Prototypes ===

// Circular Buffer functions
void init_circular_buffer(CircularBuffer* cb, int max_size);
void free_circular_buffer(CircularBuffer* cb);
void append_circular_buffer(CircularBuffer* cb, double value);
double* get_circular_buffer_snapshot(CircularBuffer* cb, int* actual_len);

// Data Processing functions
double* normalize_to_weights(const int* values, int num_values);
double* remove_dc_offset_temp(const double* values, int num_values, int* out_len); // Used for DSP input
void compute_fft(const double* values, int num_values, double sampling_rate,
                 double** frequencies_out, double** magnitude_out, double* dominant_frequency_out, int* fft_len_out);
void fir_filter(const double* values, int num_values, double cut_off_frequency, double sampling_rate,
                double** filtered_values_out, double** fir_coefficients_out, int* filtered_len_out, int* coeff_len_out);

// File I/O
void write_data_to_file(const char* file_name, const double* raw_weights_all, int raw_len,
                        const double* filtered_weights_all, int filtered_len,
                        const double* fir_coefficients, int fir_coeff_len,
                        const double* fft_frequencies_last, int fft_freq_len,
                        const double* fft_magnitude_last, int fft_mag_len);

// Network functions
int recvall(SOCKET sock, void* buffer, size_t len);
DWORD WINAPI network_thread_func(LPVOID lpParam); // Network thread entry point

// GTK specific GUI functions and callbacks
gboolean draw_raw_plot_callback(GtkWidget *widget, cairo_t *cr, gpointer data);
gboolean draw_filtered_plot_callback(GtkWidget *widget, cairo_t *cr, gpointer data);
gboolean draw_fft_plot_callback(GtkWidget *widget, cairo_t *cr, gpointer data);
void update_all_plots_gui(); // Function to trigger redraws for all plots
gboolean process_data_gui_callback(gpointer user_data); // GTK idle callback for data processing
void on_window_destroy(GtkWidget *widget, gpointer data);

// Helper for drawing common plot elements (axes, grids, labels)
void draw_plot_frame(cairo_t *cr, guint width, guint height, 
                     double margin_left, double margin_right, double margin_top, double margin_bottom,
                     double x_range_max, double y_range_min, double y_range_max,
                     const char* x_label, const char* y_label, const char* title,
                     int num_xtick_labels, int num_ytick_labels, const char* y_format);


// === Circular Buffer Implementations (unchanged) ===
void init_circular_buffer(CircularBuffer* cb, int max_size) {
    cb->data = (double*)malloc(sizeof(double) * max_size);
    if (!cb->data) { perror("Failed to allocate circular buffer data"); exit(EXIT_FAILURE); }
    cb->head = 0; cb->tail = 0; cb->count = 0; cb->max_size = max_size;
}

void free_circular_buffer(CircularBuffer* cb) {
    if (cb->data) { free(cb->data); cb->data = NULL; }
}

void append_circular_buffer(CircularBuffer* cb, double value) {
    if (cb->data == NULL) { fprintf(stderr, "Error: Circular buffer not initialized.\n"); return; }
    cb->data[cb->tail] = value;
    cb->tail = (cb->tail + 1) % cb->max_size;
    if (cb->count < cb->max_size) { cb->count++; } else { cb->head = (cb->head + 1) % cb->max_size; }
}

// Returns a dynamically allocated array containing the current elements. Caller must free.
double* get_circular_buffer_snapshot(CircularBuffer* cb, int* actual_len) {
    EnterCriticalSection(&plot_lock); // Lock to ensure consistent read
    *actual_len = cb->count;
    if (cb->count == 0) { LeaveCriticalSection(&plot_lock); return NULL; }

    double* snapshot = (double*)malloc(sizeof(double) * cb->count);
    if (!snapshot) { perror("Failed to allocate snapshot buffer"); LeaveCriticalSection(&plot_lock); return NULL; }

    if (cb->head < cb->tail) {
        memcpy(snapshot, cb->data + cb->head, cb->count * sizeof(double));
    } else { // Data wraps around
        int part1_len = cb->max_size - cb->head;
        memcpy(snapshot, cb->data + cb->head, part1_len * sizeof(double)); 
        memcpy(snapshot + part1_len, cb->data, cb->tail * sizeof(double));
    }
    LeaveCriticalSection(&plot_lock);
    return snapshot;
}

// === Data Normalization (unchanged) ===
double* normalize_to_weights(const int* values, int num_values) {
    if (num_values <= 0 || !values) return NULL;
    double* weights = (double*)malloc(sizeof(double) * num_values);
    if (!weights) { perror("Failed to allocate weights buffer"); return NULL; }

    for (int i = 0; i < num_values; ++i) {
        double data_in = (double)values[i] / ADC_MAX_VAL;
        weights[i] = (SCALE_CAL != 0) ? (data_in - ZERO_CAL) / SCALE_CAL : NAN;
    }
    return weights;
}

// === DSP Functions (Placeholders - unchanged) ===
double* remove_dc_offset_temp(const double* values, int num_values, int* out_len) {
    *out_len = num_values;
    if (num_values == 0 || !values) return NULL;

    double* result = (double*)malloc(sizeof(double) * num_values);
    if (!result) { perror("Failed to allocate result buffer for DC offset removal"); return NULL; }

    double sum = 0.0; int nan_count = 0;
    for (int i = 0; i < num_values; ++i) {
        if (!isnan(values[i])) { sum += values[i]; } else { nan_count++; }
    }

    if (num_values - nan_count == 0) { for (int i = 0; i < num_values; ++i) result[i] = NAN; return result; }
    
    double mean = sum / (num_values - nan_count);
    for (int i = 0; i < num_values; ++i) { result[i] = values[i] - mean; }
    return result;
}

void compute_fft(const double* values, int num_values, double sampling_rate,
                 double** frequencies_out, double** magnitude_out, double* dominant_frequency_out, int* fft_len_out) {
    *fft_len_out = num_values / 2;
    if (num_values < 2) {
        *frequencies_out = NULL; *magnitude_out = NULL; *dominant_frequency_out = 0.0; *fft_len_out = 0; return;
    }
    *frequencies_out = (double*)calloc(*fft_len_out, sizeof(double));
    *magnitude_out = (double*)calloc(*fft_len_out, sizeof(double));
    if (!*frequencies_out || !*magnitude_out) {
        perror("Failed to allocate FFT buffers");
        free(*frequencies_out); free(*magnitude_out);
        *frequencies_out = NULL; *magnitude_out = NULL; *dominant_frequency_out = 0.0; *fft_len_out = 0; return;
    }

    // --- PLACEHOLDER FFT IMPLEMENTATION ---
    // This provides a flat line output. Replace with real FFT algorithm (e.g., FFTW)
    // to see meaningful frequency spectrum.
    for(int i = 0; i < *fft_len_out; ++i) {
        (*frequencies_out)[i] = (double)i * sampling_rate / num_values;
        (*magnitude_out)[i] = 1.0; // Flat magnitude
    }
    *dominant_frequency_out = (*fft_len_out > 1) ? (*frequencies_out)[1] : 0.0;
    // --- END PLACEHOLDER ---
}

void fir_filter(const double* values, int num_values, double cut_off_frequency, double sampling_rate,
                double** filtered_values_out, double** fir_coefficients_out, int* filtered_len_out, int* coeff_len_out) {
    *filtered_len_out = num_values;
    *coeff_len_out = FIR_NUM_TAPS;

    if (num_values == 0 || !values || sampling_rate <= 0) {
        *filtered_values_out = NULL; *fir_coefficients_out = NULL; *filtered_len_out = 0; *coeff_len_out = 0; return;
    }

    *filtered_values_out = (double*)calloc(num_values, sizeof(double)); 
    *fir_coefficients_out = (double*)calloc(FIR_NUM_TAPS, sizeof(double));
    if (!*filtered_values_out || !*fir_coefficients_out) {
        perror("Failed to allocate FIR buffers");
        free(*filtered_values_out); free(*fir_coefficients_out);
        *filtered_values_out = NULL; *fir_coefficients_out = NULL; *filtered_len_out = 0; *coeff_len_out = 0; return;
    }
    
    // --- PLACEHOLDER FIR FILTER IMPLEMENTATION ---
    // This simply copies the input to the output. Replace with real FIR filter logic.
    for(int i = 0; i < num_values; ++i) (*filtered_values_out)[i] = values[i];
    
    // Dummy coefficients
    for(int i = 0; i < FIR_NUM_TAPS; ++i) (*fir_coefficients_out)[i] = 0.0;
    if (FIR_NUM_TAPS > 0) (*fir_coefficients_out)[FIR_NUM_TAPS / 2] = 1.0; // Impulse response for no-op filter
    // --- END PLACEHOLDER ---
}

// === Data Saving Function (unchanged) ===
void write_data_to_file(const char* file_name, const double* raw_weights_all, int raw_len,
                        const double* filtered_weights_all, int filtered_len,
                        const double* fir_coefficients, int fir_coeff_len,
                        const double* fft_frequencies_last, int fft_freq_len,
                        const double* fft_magnitude_last, int fft_mag_len) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "output_data\\all_data_%s.txt", file_name);

    _mkdir("output_data"); // Creates the directory if it doesn't exist

    printf("[CLIENT] Attempting to write data to %s\n", filepath);

    FILE* fp = fopen(filepath, "w");
    if (fp == NULL) { perror("[CLIENT] Error opening output file"); return; }

    fprintf(fp, "Raw Weights (total %d samples):\n[", raw_len);
    for (int i = 0; i < raw_len; ++i) { fprintf(fp, "%.6f%s", raw_weights_all[i], (i == raw_len - 1) ? "" : ", "); }
    fprintf(fp, "]\n\n");

    fprintf(fp, "Filtered Weights (total %d samples):\n[", filtered_len);
    for (int i = 0; i < filtered_len; ++i) { fprintf(fp, "%.6f%s", filtered_weights_all[i], (i == filtered_len - 1) ? "" : ", "); }
    fprintf(fp, "]\n\n");
    
    fprintf(fp, "FIR Coefficients (total %d samples):\n[", fir_coeff_len);
    if (fir_coeff_len > 0) { for (int i = 0; i < fir_coeff_len; ++i) { fprintf(fp, "%.6f%s", fir_coefficients[i], (i == fir_coeff_len - 1) ? "" : ", "); } } else { fprintf(fp, "N/A"); }
    fprintf(fp, "]\n\n");

    fprintf(fp, "FFT Frequencies (last computed window, total %d samples):\n[", fft_freq_len);
    if (fft_freq_len > 0) { for (int i = 0; i < fft_freq_len; ++i) { fprintf(fp, "%.6f%s", fft_frequencies_last[i], (i == fft_freq_len - 1) ? "" : ", "); } } else { fprintf(fp, "N/A"); }
    fprintf(fp, "]\n\n");

    fprintf(fp, "FFT Magnitudes (last computed window, total %d samples):\n[", fft_mag_len);
    if (fft_mag_len > 0) { for (int i = 0; i < fft_mag_len; ++i) { fprintf(fp, "%.6f%s", fft_magnitude_last[i], (i == fft_mag_len - 1) ? "" : ", "); } } else { fprintf(fp, "N/A"); }
    fprintf(fp, "]\n\n");

    fclose(fp);
    printf("[CLIENT] Successfully wrote all data for %s to %s\n", file_name, filepath);
}

// === Network Helper Function (unchanged) ===
int recvall(SOCKET sock, void* buffer, size_t len) {
    size_t total_received = 0;
    while (total_received < len) {
        int bytes_received = recv(sock, (char*)buffer + total_received, (int)(len - total_received), 0);
        if (bytes_received <= 0) { return -1; }
        total_received += bytes_received;
    }
    return (int)total_received;
}

// === Network Thread Function (unchanged) ===
DWORD WINAPI network_thread_func(LPVOID lpParam) {
    network_thread_running = 1;
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in serv_addr;
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        fprintf(stderr, "[CLIENT] WSAStartup failed: %d\n", WSAGetLastError());
        network_thread_running = 0; return 1;
    }

    if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) {
        fprintf(stderr, "[CLIENT] Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup(); network_thread_running = 0; return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (InetPton(AF_INET, SERVER_IP, &serv_addr.sin_addr) != 1) {
        fprintf(stderr, "[CLIENT] Invalid address/ Address not supported: %d\n", WSAGetLastError());
        closesocket(sock); WSACleanup(); network_thread_running = 0; return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[CLIENT] Connection failed: %d\n", WSAGetLastError());
        closesocket(sock); WSACleanup(); network_thread_running = 0; return 1;
    }
    printf("[CLIENT] Connected to server.\n");

    int interval_ms = 20; // Default sampling interval from server config

    uint32_t config_len_network;
    if (recvall(sock, &config_len_network, CONFIG_LENGTH_BYTES) == -1) {
        printf("[CLIENT] Server disconnected while receiving config length.\n"); closesocket(sock); WSACleanup(); network_thread_running = 0; return 1;
    }
    uint32_t config_len = ntohl(config_len_network);
    char* config_data_bytes = (char*)malloc(config_len + 1);
    if (!config_data_bytes) { perror("malloc for config data"); closesocket(sock); WSACleanup(); network_thread_running = 0; return 1; }
    if (recvall(sock, config_data_bytes, config_len) == -1) {
        printf("[CLIENT] Server disconnected while receiving config data.\n"); free(config_data_bytes); closesocket(sock); WSACleanup(); network_thread_running = 0; return 1;
    }
    config_data_bytes[config_len] = '\0';
    printf("[CLIENT] Received config: %s\n", config_data_bytes);
    
    char* line = strtok(config_data_bytes, "\n"); // Use \n as delimiter from server
    while(line != NULL) {
        if (strstr(line, "INTERVAL:") != NULL) { sscanf(line, "INTERVAL:%d", &interval_ms); printf("[CLIENT] Set interval: %d ms\n", interval_ms); }
        // Add other config parsing here if needed (e.g., MODE)
        line = strtok(NULL, "\n");
    }
    free(config_data_bytes);

    while (plotting_active) { // Continue as long as GUI is active
        uint32_t filename_len_network;
        if (recvall(sock, &filename_len_network, FILENAME_LENGTH_BYTES) == -1) {
            printf("[CLIENT] Server disconnected or no more files to receive (filename length).\n"); break;
        }
        uint32_t filename_length = ntohl(filename_len_network);

        char file_name[256];
        if (filename_length >= sizeof(file_name)) {
            fprintf(stderr, "[CLIENT] Received filename too long. Truncating.\n"); filename_length = sizeof(file_name) - 1;
        }
        if (recvall(sock, file_name, filename_length) == -1) {
            printf("[CLIENT] Server disconnected while receiving filename.\n"); break;
        }
        file_name[filename_length] = '\0';
        printf("[CLIENT] Received file name: %s\n", file_name);

        if (strcmp(file_name, "END_OF_TRANSMISSION") == 0) {
            printf("[CLIENT] Received END_OF_TRANSMISSION signal from server. Stopping file reception.\n"); break;
        }
        // Handle server messages disguised as filenames
        if (strstr(file_name, "NO_FILE_FOUND:") != NULL || strcmp(file_name, "NO_FILE_SELECTED") == 0 ||
            strcmp(file_name, "NO_FILES_IN_FOLDER") == 0) {
            printf("[CLIENT] Server message: %s. Stopping file reception.\n", file_name);
            // Even if it's a message, the protocol expects content length, which should be 0.
            uint64_t dummy_content_len_net;
            if (recvall(sock, &dummy_content_len_net, FILE_CONTENT_LENGTH_BYTES) == -1) {
                printf("[CLIENT] Server disconnected while trying to read dummy content length for message.\n");
            }
            break; // Break after receiving message and its 0 content length
        }


        uint64_t file_content_len_network;
        if (recvall(sock, &file_content_len_network, FILE_CONTENT_LENGTH_BYTES) == -1) {
            printf("[CLIENT] Server disconnected while receiving file content length.\n"); break;
        }
        uint64_t file_content_length = ntohll_custom(file_content_len_network);

        printf("[CLIENT] Expecting file content of length: %llu bytes for %s\n", (unsigned long long)file_content_length, file_name);

        char* file_content_data = (char*)malloc((size_t)file_content_length + 1);
        if (!file_content_data) { perror("malloc for file content"); break; }
        if (recvall(sock, file_content_data, (size_t)file_content_length) == -1) {
            printf("[CLIENT] Server disconnected while receiving file content for %s.\n", file_name); free(file_content_data); break;
        }
        file_content_data[file_content_length] = '\0';
        printf("[CLIENT] Received file content. Actual Length: %zu bytes.\n", strlen(file_content_data));

        double* raw_adc_values = NULL;
        int raw_adc_count = 0;
        char* line_content_ptr = file_content_data; // Use a new pointer for strtok
        
        char* current_adc_line;
        while ((current_adc_line = strtok(line_content_ptr, "\n")) != NULL) { // Use '\n' for splitting
            line_content_ptr = NULL; // For subsequent calls to strtok
            if (strstr(current_adc_line, "ADC:") != NULL) {
                int adc_val;
                if (sscanf(current_adc_line, "ADC:%d", &adc_val) == 1) {
                    raw_adc_count++;
                    raw_adc_values = (double*)realloc(raw_adc_values, sizeof(double) * raw_adc_count);
                    if (!raw_adc_values) { perror("realloc for raw_adc_values"); break; }
                    raw_adc_values[raw_adc_count - 1] = (double)adc_val;
                } else { fprintf(stderr, "[CLIENT] Warning: Invalid ADC value in line: %s. Skipping.\n", current_adc_line); }
            }
        }
        
        free(file_content_data);

        if (raw_adc_values && raw_adc_count > 0) {
            EnterCriticalSection(&queue_lock);
            if (queue_count < 10) {
                QueuedData* data_item = (QueuedData*)malloc(sizeof(QueuedData));
                if (!data_item) { perror("malloc for QueuedData"); free(raw_adc_values); }
                else {
                    data_item->raw_adc_values = raw_adc_values;
                    data_item->num_samples = raw_adc_count;
                    data_item->interval_ms = interval_ms;
                    strncpy(data_item->file_name, file_name, sizeof(data_item->file_name) - 1);
                    data_item->file_name[sizeof(data_item->file_name) - 1] = '\0';

                    data_queue[queue_tail] = data_item;
                    queue_tail = (queue_tail + 1) % 10;
                    queue_count++;
                    WakeConditionVariable(&queue_cond);
                    printf("[CLIENT] Put full file '%s' into queue (%d items).\n", file_name, queue_count);
                }
            } else {
                printf("[CLIENT] Queue is full, dropping file '%s'.\n", file_name);
                free(raw_adc_values);
            }
            LeaveCriticalSection(&queue_lock);
        } else {
            printf("[CLIENT] No valid ADC values found in file %s. Not adding to queue.\n", file_name);
            free(raw_adc_values);
        }
    }

    closesocket(sock);
    printf("[CLIENT] Network connection closed.\n");
    network_thread_running = 0;
    WakeConditionVariable(&queue_cond);
    WSACleanup();
    return 0;
}

// === GTK Drawing and Update Functions ===

// Helper function to draw common plot elements like axes, grid, and labels
// This simplifies the draw_callback functions and ensures consistency.
void draw_plot_frame(cairo_t *cr, guint width, guint height, 
                     double margin_left, double margin_right, double margin_top, double margin_bottom,
                     double x_range_max, double y_range_min, double y_range_max,
                     const char* x_label, const char* y_label, const char* title,
                     int num_xtick_labels, int num_ytick_labels, const char* y_format) {
    
    const double plot_area_width = width - margin_left - margin_right;
    const double plot_area_height = height - margin_top - margin_bottom;

    // Draw background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // White background
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    // Set origin to bottom-left of plot area for easier scaling
    cairo_save(cr);
    cairo_translate(cr, margin_left, margin_top + plot_area_height);
    cairo_scale(cr, plot_area_width / x_range_max, -plot_area_height / (y_range_max - y_range_min));
    cairo_translate(cr, 0, -y_range_min); // Adjust for min_y offset

    // Draw Grid lines - Light gray
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_matrix_t current_matrix_for_width; // Declare matrix for line width calculation
    cairo_get_matrix(cr, &current_matrix_for_width); // Get current transformation matrix
    cairo_set_line_width(cr, 1.0 / current_matrix_for_width.xx); // Adjust line width to be 1 pixel regardless of zoom/scale

    // Horizontal grid lines
    for (int i = 0; i <= num_ytick_labels; ++i) {
        double y_grid_val = y_range_min + (y_range_max - y_range_min) * i / (double)num_ytick_labels;
        cairo_move_to(cr, 0, y_grid_val);
        cairo_line_to(cr, x_range_max, y_grid_val);
        cairo_stroke(cr);
    }
    // Vertical grid lines
    for (int i = 0; i <= num_xtick_labels; ++i) {
        double x_grid_val = x_range_max * i / (double)num_xtick_labels;
        cairo_move_to(cr, x_grid_val, y_range_min);
        cairo_line_to(cr, x_grid_val, y_range_max);
        cairo_stroke(cr);
    }
    cairo_restore(cr); // Restore from coordinate system for grid/data drawing


    // Draw Axes - Dark gray and thicker
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.5);

    // X-axis
    cairo_move_to(cr, margin_left, margin_top + plot_area_height);
    cairo_line_to(cr, margin_left + plot_area_width, margin_top + plot_area_height);
    cairo_stroke(cr);

    // Y-axis
    cairo_move_to(cr, margin_left, margin_top);
    cairo_line_to(cr, margin_left, margin_top + plot_area_height);
    cairo_stroke(cr);

    // Draw Ticks and Labels - Dark gray text
    cairo_set_font_size(cr, 10);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_text_extents_t extents;
    char text_buffer[50];

    // Y-axis labels and ticks
    for (int i = 0; i <= num_ytick_labels; ++i) {
        double y_val = y_range_min + (y_range_max - y_range_min) * i / (double)num_ytick_labels;
        double y_pixel = margin_top + plot_area_height - (y_val - y_range_min) / (y_range_max - y_range_min) * plot_area_height;
        cairo_move_to(cr, margin_left - 5, y_pixel); cairo_line_to(cr, margin_left, y_pixel); cairo_stroke(cr); // Tick
        snprintf(text_buffer, sizeof(text_buffer), y_format, y_val);
        cairo_text_extents(cr, text_buffer, &extents);
        cairo_move_to(cr, margin_left - extents.width - 10, y_pixel + extents.height / 2);
        cairo_show_text(cr, text_buffer);
    }

    // X-axis labels and ticks (only Sample Index label is done generally)
    // For specific x-axis tick values, you'd need more specific loops here
    int num_xtick_labels_display = 4; // Example for 0, 125, 250, 375, 500
    for (int i = 0; i <= num_xtick_labels_display; ++i) {
        double x_val = x_range_max * i / (double)num_xtick_labels_display;
        double x_pixel = margin_left + (x_val / x_range_max) * plot_area_width;
        cairo_move_to(cr, x_pixel, margin_top + plot_area_height + 5); cairo_line_to(cr, x_pixel, margin_top + plot_area_height); cairo_stroke(cr); // Tick
        snprintf(text_buffer, sizeof(text_buffer), "%.0f", x_val);
        cairo_text_extents(cr, text_buffer, &extents);
        cairo_move_to(cr, x_pixel - extents.width / 2, margin_top + plot_area_height + 20);
        cairo_show_text(cr, text_buffer);
    }

    // Plot Title - Bold and larger font
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2); 
    cairo_set_font_size(cr, 14);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_text_extents(cr, title, &extents);
    cairo_move_to(cr, margin_left + plot_area_width / 2 - extents.width / 2, margin_top - extents.height - 5); // Above plot area
    cairo_show_text(cr, title);

    // X-axis label (common for all plot types)
    cairo_set_font_size(cr, 12);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_text_extents(cr, x_label, &extents);
    cairo_move_to(cr, margin_left + plot_area_width / 2 - extents.width / 2, height - 10);
    cairo_show_text(cr, x_label);

    // Y-axis label (common for all plot types)
    cairo_save(cr);
    cairo_rotate(cr, -G_PI / 2.0); // Rotate 90 degrees counter-clockwise
    cairo_text_extents(cr, y_label, &extents);
    cairo_move_to(cr, -(margin_top + plot_area_height / 2 + extents.width / 2), margin_left - 40); // Position rotated text
    cairo_show_text(cr, y_label);
    cairo_restore(cr); // Restore from rotation
}


// --- RAW PLOT DRAW CALLBACK ---
gboolean draw_raw_plot_callback(GtkWidget *widget, cairo_t *cr, gpointer data) {
    guint width = gtk_widget_get_allocated_width(widget);
    guint height = gtk_widget_get_allocated_height(widget);

    // Plot margins
    const double margin_left = 60.0, margin_right = 20.0, margin_top = 20.0, margin_bottom = 40.0;
    const double plot_area_width = width - margin_left - margin_right;
    const double plot_area_height = height - margin_top - margin_bottom;

    // --- CRITICAL SECTION: Lock data buffers for reading ---
    EnterCriticalSection(&plot_lock);
    int raw_len;
    double* raw_data_to_plot = get_circular_buffer_snapshot(&current_raw_buffer, &raw_len);
    
    // Determine Y-axis range for raw data
    double min_y = 0.0, max_y = 700.0; 
    if (raw_data_to_plot && raw_len > 0) {
        int first_valid = -1;
        for(int i = 0; i < raw_len; ++i) { if(!isnan(raw_data_to_plot[i])) { first_valid = i; break; } }
        if(first_valid != -1) {
            min_y = raw_data_to_plot[first_valid]; max_y = raw_data_to_plot[first_valid];
            for (int i = first_valid + 1; i < raw_len; ++i) {
                if (!isnan(raw_data_to_plot[i])) {
                    if (raw_data_to_plot[i] < min_y) min_y = raw_data_to_plot[i];
                    if (raw_data_to_plot[i] > max_y) max_y = raw_data_to_plot[i];
                }
            }
        }
    }
    // Add padding to Y limits
    if (!isnan(min_y) && !isnan(max_y) && (max_y - min_y) > 1e-9) {
        double padding = (max_y - min_y) * 0.1;
        min_y -= padding;
        max_y += padding;
    } else { // Fallback if data is flat or all NaN
        min_y = -0.1; max_y = 0.1; // Default small range
    }

    // Draw frame, axes, grids, and labels
    draw_plot_frame(cr, width, height, margin_left, margin_right, margin_top, margin_bottom,
                    PLOT_BUFFER_SIZE - 1.0, min_y, max_y,
                    "Sample Index", "Weight", g_strdup_printf("Raw ADC Data - %s", g_file_state.current_file_name),
                    4, 5, "%.0f"); // Use %.0f for integer-like weight labels

    // Plot Raw Data (Red)
    if (raw_data_to_plot && raw_len > 1) {
        cairo_save(cr); // Save state for plot drawing transformations
        cairo_translate(cr, margin_left, margin_top + plot_area_height);
        cairo_scale(cr, plot_area_width / (PLOT_BUFFER_SIZE - 1.0), -plot_area_height / (max_y - min_y));
        cairo_translate(cr, 0, -min_y);

        cairo_matrix_t current_matrix;
        cairo_get_matrix(cr, &current_matrix);
        cairo_set_line_width(cr, 1.5 / current_matrix.xx); // Line thickness for data

        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0); // Red
        int first_valid = -1;
        for(int i = 0; i < raw_len; ++i) { if(!isnan(raw_data_to_plot[i])) { first_valid = i; break; } }
        if(first_valid != -1) {
            cairo_move_to(cr, (double)first_valid, raw_data_to_plot[first_valid]);
            for (int i = first_valid + 1; i < raw_len; ++i) {
                if (!isnan(raw_data_to_plot[i])) { cairo_line_to(cr, (double)i, raw_data_to_plot[i]); }
                else { cairo_move_to(cr, (double)i, raw_data_to_plot[i-1]); }
            }
            cairo_stroke(cr);
        }
        cairo_restore(cr); // Restore transformations
    }

    // Free snapshots
    if (raw_data_to_plot) free(raw_data_to_plot);

    LeaveCriticalSection(&plot_lock);
    return FALSE;
}

// --- FILTERED PLOT DRAW CALLBACK ---
gboolean draw_filtered_plot_callback(GtkWidget *widget, cairo_t *cr, gpointer data) {
    guint width = gtk_widget_get_allocated_width(widget);
    guint height = gtk_widget_get_allocated_height(widget);

    const double margin_left = 60.0, margin_right = 20.0, margin_top = 20.0, margin_bottom = 40.0;
    const double plot_area_width = width - margin_left - margin_right;
    const double plot_area_height = height - margin_top - margin_bottom;

    EnterCriticalSection(&plot_lock);
    int filtered_len;
    double* filtered_data_to_plot = get_circular_buffer_snapshot(&current_filtered_buffer, &filtered_len);

    // Determine Y-axis range for filtered data (should be centered around 0)
    double min_y = -0.1, max_y = 0.1; // Default small range, adjust if data varies more
    if (filtered_data_to_plot && filtered_len > 0) {
        int first_valid = -1;
        for(int i = 0; i < filtered_len; ++i) { if(!isnan(filtered_data_to_plot[i])) { first_valid = i; break; } }
        if(first_valid != -1) {
            min_y = filtered_data_to_plot[first_valid]; max_y = filtered_data_to_plot[first_valid];
            for (int i = first_valid + 1; i < filtered_len; ++i) {
                if (!isnan(filtered_data_to_plot[i])) {
                    if (filtered_data_to_plot[i] < min_y) min_y = filtered_data_to_plot[i];
                    if (filtered_data_to_plot[i] > max_y) max_y = filtered_data_to_plot[i];
                }
            }
        }
    }
    // Add padding and ensure centered around 0 (or close to it)
    if (!isnan(min_y) && !isnan(max_y) && (max_y - min_y) > 1e-9) {
        double abs_max = fmax(fabs(min_y), fabs(max_y));
        min_y = -abs_max * 1.1; // 10% padding
        max_y = abs_max * 1.1;
        if (max_y - min_y < 1e-9) { min_y = -0.1; max_y = 0.1; } // Fallback
    } else {
        min_y = -0.1; max_y = 0.1; // Default
    }

    // Draw frame, axes, grids, and labels
    draw_plot_frame(cr, width, height, margin_left, margin_right, margin_top, margin_bottom,
                    PLOT_BUFFER_SIZE - 1.0, min_y, max_y,
                    "Sample Index", "Weight", g_strdup_printf("FIR-Filtered ADC Data - %s", g_file_state.current_file_name),
                    4, 4, "%.2f"); // Use %.2f for decimal weight labels

    // Plot Filtered Data (Green)
    if (filtered_data_to_plot && filtered_len > 1) {
        cairo_save(cr);
        cairo_translate(cr, margin_left, margin_top + plot_area_height);
        cairo_scale(cr, plot_area_width / (PLOT_BUFFER_SIZE - 1.0), -plot_area_height / (max_y - min_y));
        cairo_translate(cr, 0, -min_y);

        cairo_matrix_t current_matrix;
        cairo_get_matrix(cr, &current_matrix);
        cairo_set_line_width(cr, 1.5 / current_matrix.xx);

        cairo_set_source_rgb(cr, 0.0, 0.8, 0.0); // Green for contrast
        int first_valid = -1;
        for(int i = 0; i < filtered_len; ++i) { if(!isnan(filtered_data_to_plot[i])) { first_valid = i; break; } }
        if(first_valid != -1) {
            cairo_move_to(cr, (double)first_valid, filtered_data_to_plot[first_valid]);
            for (int i = first_valid + 1; i < filtered_len; ++i) {
                if (!isnan(filtered_data_to_plot[i])) { cairo_line_to(cr, (double)i, filtered_data_to_plot[i]); }
                else { cairo_move_to(cr, (double)i, filtered_data_to_plot[i-1]); }
            }
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }

    if (filtered_data_to_plot) free(filtered_data_to_plot);
    LeaveCriticalSection(&plot_lock);
    return FALSE;
}

// --- FFT PLOT DRAW CALLBACK ---
gboolean draw_fft_plot_callback(GtkWidget *widget, cairo_t *cr, gpointer data) {
    guint width = gtk_widget_get_allocated_width(widget);
    guint height = gtk_widget_get_allocated_height(widget);

    const double margin_left = 60.0, margin_right = 20.0, margin_top = 20.0, margin_bottom = 40.0;
    const double plot_area_width = width - margin_left - margin_right;
    const double plot_area_height = height - margin_top - margin_bottom;

    EnterCriticalSection(&plot_lock);
    // Get FFT data (already stored in g_file_state from last computation in process_data_gui_callback)
    double* fft_freqs = g_file_state.last_fft_frequencies_to_save;
    double* fft_mags = g_file_state.last_fft_magnitude_to_save;
    int fft_plot_len = g_file_state.last_fft_frequencies_len_to_save;

    // Determine Y-axis range for FFT plot
    double min_y = 0.0, max_y = 1.0; // Default range
    if (fft_mags && fft_plot_len > 1) { // Skip DC component (index 0) for scaling
        max_y = fft_mags[1]; // Start from first non-DC component
        for (int i = 2; i < fft_plot_len; ++i) { 
            if (!isnan(fft_mags[i]) && fft_mags[i] > max_y) max_y = fft_mags[i];
        }
    }
    if (max_y < 1e-9) max_y = 1.0; // Avoid zero range
    double y_padding = max_y * 0.1;
    min_y = 0.0; // FFT magnitude is always non-negative
    max_y += y_padding;

    // Determine X-axis range for FFT plot (up to Nyquist or max freq in data)
    double max_x = 0.0;
    if (fft_freqs && fft_plot_len > 0) {
        max_x = fft_freqs[fft_plot_len - 1]; // Max frequency is last element
    }
    if (max_x < 1e-9) max_x = 100.0; // Default to 100Hz if no data

    // Draw frame, axes, grids, and labels
    draw_plot_frame(cr, width, height, margin_left, margin_right, margin_top, margin_bottom,
                    max_x, min_y, max_y,
                    "Frequency (Hz)", "Magnitude", g_strdup_printf("FFT Spectrum - %s (Placeholder)", g_file_state.current_file_name),
                    4, 4, "%.0e"); // Use scientific notation for magnitude labels

    // Plot FFT Data (Blue)
    if (fft_freqs && fft_mags && fft_plot_len > 1) {
        cairo_save(cr);
        cairo_translate(cr, margin_left, margin_top + plot_area_height);
        cairo_scale(cr, plot_area_width / max_x, -plot_area_height / (max_y - min_y));
        cairo_translate(cr, 0, -min_y);

        cairo_matrix_t current_matrix;
        cairo_get_matrix(cr, &current_matrix);
        cairo_set_line_width(cr, 1.5 / current_matrix.xx);

        cairo_set_source_rgb(cr, 0.0, 0.0, 1.0); // Blue
        int first_valid = -1;
        for(int i = 1; i < fft_plot_len; ++i) { // Start from 1 to avoid DC component at 0Hz
            if(!isnan(fft_mags[i])) {
                first_valid = i;
                break;
            }
        }
        if(first_valid != -1) {
            cairo_move_to(cr, fft_freqs[first_valid], fft_mags[first_valid]);
            for (int i = first_valid + 1; i < fft_plot_len; ++i) {
                if (!isnan(fft_mags[i])) { cairo_line_to(cr, fft_freqs[i], fft_mags[i]); }
                else { cairo_move_to(cr, fft_freqs[i], fft_mags[i-1]); }
            }
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }

    LeaveCriticalSection(&plot_lock);
    return FALSE;
}

// Function to trigger redraws for all three plot areas
void update_all_plots_gui() {
    if (raw_plot_area) gtk_widget_queue_draw(raw_plot_area);
    if (filtered_plot_area) gtk_widget_queue_draw(filtered_plot_area);
    if (fft_plot_area) gtk_widget_queue_draw(fft_plot_area);
}

// GTK Idle/Timeout callback to process data from the network thread's queue and update GUI
gboolean process_data_gui_callback(gpointer user_data) {
    // If a file is currently being processed (by this timeout callback)
    if (g_file_state.is_processing_file) {
        // Check if there are more samples to process in the current file
        if (g_file_state.current_file_index < g_file_state.current_file_num_samples) {
            double current_raw_adc = g_file_state.current_file_raw_adc_values[g_file_state.current_file_index];
            
            // --- CRITICAL SECTION: Lock buffers for writing data ---
            EnterCriticalSection(&plot_lock);
            
            append_circular_buffer(&dsp_raw_adc_buffer, current_raw_adc); 

            // Normalize current raw ADC value to weight
            int temp_adc = (int)current_raw_adc;
            double* current_raw_weight_arr = normalize_to_weights(&temp_adc, 1);
            double current_raw_weight = NAN;
            if (current_raw_weight_arr) {
                current_raw_weight = current_raw_weight_arr[0];
                free(current_raw_weight_arr);
            }
            append_circular_buffer(&current_raw_buffer, current_raw_weight); // Raw data (with DC) for raw plot
            
            // Append to all_raw_weights (for saving later)
            g_file_state.all_raw_weights_len_to_save++;
            g_file_state.all_raw_weights_to_save = (double*)realloc(g_file_state.all_raw_weights_to_save, sizeof(double) * g_file_state.all_raw_weights_len_to_save);
            if (!g_file_state.all_raw_weights_to_save) { perror("realloc failed for all_raw_weights_to_save"); goto cleanup_current_file_on_error; }
            g_file_state.all_raw_weights_to_save[g_file_state.all_raw_weights_len_to_save - 1] = current_raw_weight;

            double filtered_weight_to_save = NAN; // For saving (DC-retained or DC-removed depending on philosophy)
            double filtered_weight_for_plot = NAN; // For plotting (DC-removed)

            double sampling_rate = (g_file_state.current_file_interval_ms > 0) ? (1000.0 / g_file_state.current_file_interval_ms) : 1.0;
            int min_dsp_samples = (FIR_NUM_TAPS > FFT_WINDOW_SIZE) ? FIR_NUM_TAPS : FFT_WINDOW_SIZE;

            // Perform DSP only if enough data is available in the DSP buffer
            if (dsp_raw_adc_buffer.count >= min_dsp_samples) {
                int dsp_snapshot_len;
                double* current_dsp_raw_values_snapshot = get_circular_buffer_snapshot(&dsp_raw_adc_buffer, &dsp_snapshot_len);
                if (!current_dsp_raw_values_snapshot) { printf("[CLIENT] Error: Could not get DSP buffer snapshot.\n"); goto end_processing_loop_return_continue; } // Modified goto
                
                int processed_len_dc_removed;
                double* processed_window_dc_removed = remove_dc_offset_temp(current_dsp_raw_values_snapshot, dsp_snapshot_len, &processed_len_dc_removed);
                free(current_dsp_raw_values_snapshot); // Free snapshot after use

                if (processed_window_dc_removed) {
                    // Compute FFT and store results for plotting and saving
                    free(g_file_state.last_fft_frequencies_to_save); g_file_state.last_fft_frequencies_to_save = NULL;
                    free(g_file_state.last_fft_magnitude_to_save); g_file_state.last_fft_magnitude_to_save = NULL;
                    
                    double dom_freq;
                    compute_fft(processed_window_dc_removed, processed_len_dc_removed, sampling_rate,
                                &g_file_state.last_fft_frequencies_to_save, &g_file_state.last_fft_magnitude_to_save, &dom_freq, &g_file_state.last_fft_frequencies_len_to_save);
                    g_file_state.last_fft_magnitude_len_to_save = g_file_state.last_fft_frequencies_len_to_save;

                    // Apply FIR filter and store coefficients for saving
                    free(g_file_state.last_fir_coefficients_to_save); g_file_state.last_fir_coefficients_to_save = NULL;
                    double* filtered_segment_values_dc_removed = NULL;
                    int filtered_segment_len;
                    fir_filter(processed_window_dc_removed, processed_len_dc_removed, dom_freq, sampling_rate,
                            &filtered_segment_values_dc_removed, &g_file_state.last_fir_coefficients_to_save, &filtered_segment_len, &g_file_state.last_fir_coefficients_len_to_save);
                    
                    if (filtered_segment_values_dc_removed && filtered_segment_len > 0) {
                        double filtered_point_dc_removed = filtered_segment_values_dc_removed[filtered_segment_len - 1];
                        
                        // For plotting, use the DC-removed filtered data:
                        filtered_weight_for_plot = filtered_point_dc_removed; 

                        // For saving, typically you save what you plot (DC-removed in this case) or original DC-retained.
                        // Based on Matplotlib example's filtered plot, it's DC-removed.
                        filtered_weight_to_save = filtered_weight_for_plot; 
                        
                    } else { filtered_weight_for_plot = current_raw_weight; filtered_weight_to_save = current_raw_weight; } // Fallback
                    free(processed_window_dc_removed);
                    if(filtered_segment_values_dc_removed) free(filtered_segment_values_dc_removed); // Free filtered segment
                } else { filtered_weight_for_plot = current_raw_weight; filtered_weight_to_save = current_raw_weight; }
            } else { filtered_weight_for_plot = current_raw_weight; filtered_weight_to_save = current_raw_weight; } // Not enough data for DSP yet

            // Append filtered weight (DC-removed) to buffer for plotting
            if (dsp_raw_adc_buffer.count >= FIR_NUM_TAPS) { append_circular_buffer(&current_filtered_buffer, filtered_weight_for_plot); }
            else { append_circular_buffer(&current_filtered_buffer, NAN); } // Append NaN if not enough for filter
            
            // Append to all_filtered_weights for saving
            g_file_state.all_filtered_weights_len_to_save++;
            g_file_state.all_filtered_weights_to_save = (double*)realloc(g_file_state.all_filtered_weights_to_save, sizeof(double) * g_file_state.all_filtered_weights_len_to_save);
            if (!g_file_state.all_filtered_weights_to_save) { perror("realloc failed for all_filtered_weights_to_save"); goto cleanup_current_file_on_error; }
            g_file_state.all_filtered_weights_to_save[g_file_state.all_filtered_weights_len_to_save - 1] = filtered_weight_to_save;

            // --- END CRITICAL SECTION ---
            LeaveCriticalSection(&plot_lock);

            update_all_plots_gui(); // Request GUI redraw for all plots
            g_file_state.current_file_index++; // Move to next sample

            // Update status label in GUI
            char status_text[512]; // Increased buffer size to prevent truncation
            snprintf(status_text, sizeof(status_text), "Processing %s: Sample %d/%d", 
                     g_file_state.current_file_name, g_file_state.current_file_index, g_file_state.current_file_num_samples);
            gtk_label_set_text(GTK_LABEL(label_status), status_text);

            return G_SOURCE_CONTINUE; // Keep this timeout active for next sample
        } else {
            // Finished processing all samples for the current file
            printf("[CLIENT] Finished processing file %s. Saving data.\n", g_file_state.current_file_name);
            write_data_to_file(g_file_state.current_file_name, 
                               g_file_state.all_raw_weights_to_save, g_file_state.all_raw_weights_len_to_save,
                               g_file_state.all_filtered_weights_to_save, g_file_state.all_filtered_weights_len_to_save,
                               g_file_state.last_fir_coefficients_to_save, g_file_state.last_fir_coefficients_len_to_save,
                               g_file_state.last_fft_frequencies_to_save, g_file_state.last_fft_frequencies_len_to_save,
                               g_file_state.last_fft_magnitude_to_save, g_file_state.last_fft_magnitude_len_to_save);
            
cleanup_current_file_on_error: // Label for goto in case of realloc failure or error during processing
            // Free resources specific to the just-processed file
            free(g_file_state.current_file_raw_adc_values); g_file_state.current_file_raw_adc_values = NULL;
            free(g_file_state.all_raw_weights_to_save); g_file_state.all_raw_weights_to_save = NULL;
            free(g_file_state.all_filtered_weights_to_save); g_file_state.all_filtered_weights_to_save = NULL;
            free(g_file_state.last_fir_coefficients_to_save); g_file_state.last_fir_coefficients_to_save = NULL;
            free(g_file_state.last_fft_frequencies_to_save); g_file_state.last_fft_frequencies_to_save = NULL;
            free(g_file_state.last_fft_magnitude_to_save); g_file_state.last_fft_magnitude_to_save = NULL;
            
            memset(&g_file_state, 0, sizeof(FileProcessingState)); // Clear remaining state variables
            g_file_state.is_processing_file = 0; // Mark as no file being processed

            gtk_label_set_text(GTK_LABEL(label_status), "Finished processing file. Checking for next data.");
            
            // Remove the timeout for the current file as it's finished
            if (data_processing_source_id != 0) {
                g_source_remove(data_processing_source_id);
                data_processing_source_id = 0;
            }
            // Now, fall through to check the network queue for a new file
        }
    }

end_processing_loop_return_continue: // Modified label for goto, always returns G_SOURCE_CONTINUE if not quitting
    // If no file is currently being processed, check the network thread's queue for a new file
    EnterCriticalSection(&queue_lock);
    if (queue_count > 0) {
        QueuedData* data_item = data_queue[queue_head];
        queue_head = (queue_head + 1) % 10;
        queue_count--;
        printf("[CLIENT MAIN] Pulled full file '%s' from queue. (%d remaining)\n", data_item->file_name, queue_count);

        // Prepare g_file_state for processing the new file
        memset(&g_file_state, 0, sizeof(FileProcessingState)); // Clear previous state
        g_file_state.current_file_raw_adc_values = data_item->raw_adc_values; // Take ownership
        g_file_state.current_file_num_samples = data_item->num_samples;
        g_file_state.current_file_interval_ms = data_item->interval_ms;
        strncpy(g_file_state.current_file_name, data_item->file_name, sizeof(g_file_state.current_file_name) - 1);
        g_file_state.current_file_name[sizeof(g_file_state.current_file_name) - 1] = '\0';
        g_file_state.current_file_index = 0;
        g_file_state.is_processing_file = 1;

        // Clear circular buffers for the new file's data
        EnterCriticalSection(&plot_lock); // Must lock when modifying shared buffers
        current_raw_buffer.head = current_raw_buffer.tail = current_raw_buffer.count = 0;
        current_filtered_buffer.head = current_filtered_buffer.tail = current_filtered_buffer.count = 0;
        dsp_raw_adc_buffer.head = dsp_raw_adc_buffer.tail = dsp_raw_adc_buffer.count = 0;
        LeaveCriticalSection(&plot_lock);

        free(data_item); // Free the QueuedData wrapper struct
        LeaveCriticalSection(&queue_lock);
        gtk_label_set_text(GTK_LABEL(label_status), "Starting new file processing...");
        
        // Start a new timeout for the processing of this file
        data_processing_source_id = g_timeout_add(g_file_state.current_file_interval_ms, process_data_gui_callback, NULL);
        if (data_processing_source_id == 0) {
            fprintf(stderr, "[CLIENT MAIN] Failed to add g_timeout_add for new file processing.\n");
            // Consider error handling like quitting or retrying
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }

        return G_SOURCE_CONTINUE; // Continue from this call to allow new timeout to take over
    }
    LeaveCriticalSection(&queue_lock);

    // If no file to process and network thread is done, it's time to quit the GTK main loop
    if (!network_thread_running && queue_count == 0 && !g_file_state.is_processing_file) {
        printf("[CLIENT MAIN] No more data from network and queue is empty. Quitting GTK main loop.\n");
        gtk_main_quit(); // Exits the GTK event loop
        return G_SOURCE_REMOVE; // Stop this idle callback
    }

    // Return G_SOURCE_REMOVE from this call. If there's no file processing and no new file,
    // this idle/timeout should not continue to be called. It should only be active if
    // a file is being processed or newly available.
    // The previous logic had a g_idle_add that ran constantly. Now, the timeout
    // gets added only when a file is ready. If it returns here and no timeout is active,
    // it effectively stops itself.
    if (data_processing_source_id != 0) {
        // This case indicates an active timeout that should continue.
        return G_SOURCE_CONTINUE; 
    } else {
        // This means no active processing and no new files, so this GSource can be removed.
        return G_SOURCE_REMOVE;
    }
}


// Callback for when the main window is closed by the user
void on_window_destroy(GtkWidget *widget, gpointer data) {
    plotting_active = 0; // Set flag to stop network and processing threads
    WakeConditionVariable(&queue_cond); // Wake up network thread in case it's waiting on queue_cond
    
    // If there's an active timeout, remove it
    if (data_processing_source_id != 0) {
        g_source_remove(data_processing_source_id);
        data_processing_source_id = 0;
    }

    gtk_main_quit(); // Quit the GTK main loop, which will eventually lead to main() cleanup
}


// === Main Program ===
int main(int argc, char *argv[]) {
    // 1. Initialize GTK toolkit
    gtk_init(&argc, &argv);

    // 2. Initialize Windows synchronization primitives
    InitializeCriticalSection(&plot_lock);
    InitializeCriticalSection(&queue_lock);
    InitializeConditionVariable(&queue_cond);

    // 3. Initialize circular data buffers
    init_circular_buffer(&current_raw_buffer, PLOT_BUFFER_SIZE);
    init_circular_buffer(&current_filtered_buffer, PLOT_BUFFER_SIZE);
    init_circular_buffer(&dsp_raw_adc_buffer, DSP_BUFFER_SIZE);

    // 4. Set plotting active flag (signals threads to run)
    plotting_active = 1;

    // 5. Create main GTK window and widgets
    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), "Live ADC Data Plotter");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 800, 750); // Increased height for 3 plots
    g_signal_connect(main_window, "destroy", G_CALLBACK(on_window_destroy), NULL); // Connect window close event

    // Use a GtkBox for vertical arrangement of the 3 plot areas + status label
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);

    // Raw Plot Area
    raw_plot_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(raw_plot_area, 780, 200); // Set preferred height
    gtk_box_pack_start(GTK_BOX(vbox), raw_plot_area, FALSE, FALSE, 0); // FALSE, FALSE for fixed size
    g_signal_connect(raw_plot_area, "draw", G_CALLBACK(draw_raw_plot_callback), NULL);

    // Filtered Plot Area
    filtered_plot_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(filtered_plot_area, 780, 200);
    gtk_box_pack_start(GTK_BOX(vbox), filtered_plot_area, FALSE, FALSE, 0);
    g_signal_connect(filtered_plot_area, "draw", G_CALLBACK(draw_filtered_plot_callback), NULL);

    // FFT Plot Area
    fft_plot_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(fft_plot_area, 780, 200);
    gtk_box_pack_start(GTK_BOX(vbox), fft_plot_area, FALSE, FALSE, 0);
    g_signal_connect(fft_plot_area, "draw", G_CALLBACK(draw_fft_plot_callback), NULL);

    // Status label at the bottom
    label_status = gtk_label_new("Initializing...");
    gtk_box_pack_start(GTK_BOX(vbox), label_status, FALSE, FALSE, 0);

    // 6. Show all created widgets
    gtk_widget_show_all(main_window);

    // 7. Start the network thread (runs in background to receive data)
    HANDLE network_thread_handle;
    network_thread_handle = CreateThread(
        NULL, 0, network_thread_func, NULL, 0, NULL);
    if (network_thread_handle == NULL) {
        fprintf(stderr, "Error creating network thread.\n");
        DeleteCriticalSection(&plot_lock);
        DeleteCriticalSection(&queue_lock);
        return 1;
    }

    // 8. Add an initial idle callback to *kickstart* the processing.
    // This idle callback will run once, check if there's data, and if so,
    // it will set up the first g_timeout_add for processing.
    // If no data is immediately available, it will remove itself.
    g_idle_add(process_data_gui_callback, NULL);

    // 9. Start the GTK main loop
    gtk_main();

    // 10. Program cleanup (executed after gtk_main() exits, typically on window close)
    printf("[CLIENT] GTK main loop exited. Starting cleanup...\n");

    // Signal network thread one last time in case it's blocked, then wait for it to exit
    WakeConditionVariable(&queue_cond);
    WaitForSingleObject(network_thread_handle, INFINITE);
    CloseHandle(network_thread_handle);

    // Free all dynamically allocated circular buffer data
    free_circular_buffer(&current_raw_buffer);
    free_circular_buffer(&current_filtered_buffer);
    free_circular_buffer(&dsp_raw_adc_buffer); 
    
    // Free any remaining data associated with g_file_state (if a file was being processed when GUI closed)
    free(g_file_state.current_file_raw_adc_values); 
    free(g_file_state.all_raw_weights_to_save);
    free(g_file_state.all_filtered_weights_to_save);
    free(g_file_state.last_fir_coefficients_to_save);
    free(g_file_state.last_fft_frequencies_to_save);
    free(g_file_state.last_fft_magnitude_to_save);

    // Delete Windows synchronization primitives
    DeleteCriticalSection(&plot_lock);
    DeleteCriticalSection(&queue_lock);

    printf("[CLIENT] Exiting client application.\n");

    return 0;
}