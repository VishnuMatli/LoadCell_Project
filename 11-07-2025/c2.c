#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For close(), usleep()
#include <arpa/inet.h>  // For sockaddr_in, htons, inet_pton, inet_ntop
#include <sys/socket.h> // For socket, bind, listen, accept
#include <sys/stat.h>   // For stat(), mkdir
#include <math.h>       // For fmin, M_PI (if needed for coefficient design)
#include <stdint.h>     // For uint32_t, uint64_t
#include <endian.h>     // For be64toh (if available, otherwise custom)

// --- CMSIS-DSP Library Includes ---
// IMPORTANT: You MUST have the CMSIS-DSP library headers and compiled library
// available on your system for these includes and functions to work.
// Typical installation on Linux involves downloading CMSIS-DSP source,
// compiling it for your target (e.g., x86 for Kali), and ensuring headers/libs
// are in GCC's search path.
#include "arm_math.h" // Main CMSIS-DSP header


// Configuration
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999
#define BUFFER_SIZE 4096

// Define constants for length-prefixing (same as Python)
#define FILENAME_LENGTH_BYTES 4
#define FILE_CONTENT_LENGTH_BYTES 8
#define CONFIG_LENGTH_BYTES 4

// Calibration constants (UPDATED as per Python client request)
#define ZERO_CAL -0.0006981067708 
#define SCALE_CAL 0.00000452466566

// FIR Filter Order (Number of taps for CMSIS-DSP FIR)
#define FIR_NUM_TAPS 51 

// Global CMSIS-DSP FIR instance and state buffer
// This filter instance and its state persist across calls to process_data
// if you were processing chunks of a larger stream.
// For processing an entire file at once, the state buffer size should be
// (numTaps + blockSize - 1). Max blockSize will be raw_count.
arm_fir_instance_f32 S_fir;
// Allocate state buffer for the maximum possible block size + taps
// Assuming raw_count won't exceed BUFFER_SIZE from socket recv_all.
float32_t firState_f32[FIR_NUM_TAPS + BUFFER_SIZE - 1]; 


// Function prototypes
ssize_t recv_all(int sockfd, void *buf, size_t len);
void process_data(const char *file_content, const char *filename, int interval_ms);
double normalize_to_weight(long adc_value);
float32_t calculate_mean_f32(float32_t *data, int count); // Mean for float32_t data


// Helper for be64toh (big-endian 64-bit to host) if not directly available
#if !defined(be64toh) && (__BYTE_ORDER == __LITTLE_ENDIAN)
static inline uint64_t be64toh_custom(uint64_t big_endian_val) {
    return ((uint64_t)ntohl((uint32_t)(big_endian_val & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)(big_endian_val >> 32));
}
#define be64toh be64toh_custom
#elif !defined(be64toh)
#define be64toh(x) (x) 
#endif


int main() {
    int client_sock;
    struct sockaddr_in server_addr;

    // 1. Create socket
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0) {
        perror("Error creating socket");
        exit(EXIT_FAILURE);
    }

    // 2. Connect to server
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) { 
        perror("Invalid address/ Address not supported");
        close(client_sock);
        exit(EXIT_FAILURE);
    }

    if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error connecting to server");
        close(client_sock);
        exit(EXIT_FAILURE);
    }
    printf("Connected to server.\n");

    // --- Phase 1: Receive Initial Configuration ---
    uint32_t net_config_len;
    if (recv_all(client_sock, &net_config_len, CONFIG_LENGTH_BYTES) <= 0) {
        printf("Server disconnected while receiving config length.\n");
        close(client_sock);
        exit(EXIT_FAILURE);
    }
    size_t config_len = ntohl(net_config_len); // Convert from network byte order

    char *config_data = (char *)malloc(config_len + 1);
    if (config_data == NULL) {
        perror("Failed to allocate memory for config data");
        close(client_sock);
        exit(EXIT_FAILURE);
    }
    if (recv_all(client_sock, config_data, config_len) <= 0) {
        printf("Server disconnected while receiving config data.\n");
        free(config_data);
        close(client_sock);
        exit(EXIT_FAILURE);
    }
    config_data[config_len] = '\0'; // Null-terminate the string
    printf("Received config: %s\n", config_data);

    // Parse interval and mode (simplified parsing for C)
    int interval_ms = 20; // Default
    char mode[50] = "interval"; // Default
    char *interval_ptr = strstr(config_data, "INTERVAL:");
    if (interval_ptr) {
        sscanf(interval_ptr, "INTERVAL:%d", &interval_ms);
    }
    char *mode_ptr = strstr(config_data, "MODE:");
    if (mode_ptr) {
        sscanf(mode_ptr, "MODE:%s", mode);
    }
    printf("Set interval: %d ms, Mode: %s\n", interval_ms, mode);
    free(config_data);

    // --- Phase 2: Receive File Data ---
    while (1) {
        uint32_t net_filename_len;
        if (recv_all(client_sock, &net_filename_len, FILENAME_LENGTH_BYTES) <= 0) {
            printf("Server disconnected or no more files (filename length).\n");
            break;
        }
        size_t filename_len = ntohl(net_filename_len);

        char *filename = (char *)malloc(filename_len + 1);
        if (filename == NULL) {
            perror("Failed to allocate memory for filename");
            break;
        }
        if (recv_all(client_sock, filename, filename_len) <= 0) {
            printf("Server disconnected while receiving filename.\n");
            free(filename);
            break;
        }
        filename[filename_len] = '\0';
        printf("Received file name: %s\n", filename);

        uint64_t net_file_content_len;
        if (recv_all(client_sock, &net_file_content_len, FILE_CONTENT_LENGTH_BYTES) <= 0) {
            printf("Server disconnected while receiving file content length.\n");
            free(filename);
            break;
        }
        size_t file_content_len = be64toh(net_file_content_len); 

        // Handle control messages
        if (strcmp(filename, "END_OF_TRANSMISSION") == 0 ||
            strstr(filename, "NO_FILE_FOUND:") != NULL ||
            strcmp(filename, "NO_FILE_SELECTED") == 0 ||
            strcmp(filename, "NO_FILES_IN_FOLDER") == 0) {
            printf("Received control message: %s. Stopping file reception.\n", filename);
            free(filename);
            break; // Exit loop for control messages
        }

        printf("Expecting file content of length: %lu bytes for %s\n", (unsigned long)file_content_len, filename);

        char *file_content = (char *)malloc(file_content_len + 1);
        if (file_content == NULL) {
            perror("Failed to allocate memory for file content");
            free(filename);
            break;
        }
        if (recv_all(client_sock, file_content, file_content_len) <= 0) {
            printf("Server disconnected while receiving file content for %s.\n", filename);
            free(filename);
            free(file_content);
            break;
        }
        file_content[file_content_len] = '\0'; 

        // Process data
        process_data(file_content, filename, interval_ms);

        free(filename);
        free(file_content);
    }

    printf("Connection closed.\n");
    close(client_sock);
    return 0;
}

// Helper function to ensure all bytes are received
ssize_t recv_all(int sockfd, void *buf, size_t len) {
    size_t total_received = 0;
    ssize_t bytes_received;
    while (total_received < len) {
        bytes_received = recv(sockfd, (char *)buf + total_received, len - total_received, 0);
        if (bytes_received <= 0) {
            return bytes_received; // Error or connection closed
        }
        total_received += bytes_received;
    }
    return total_received;
}

// Processes the received data (FIR filter using CMSIS-DSP)
void process_data(const char *file_content, const char *filename, int interval_ms) {
    printf("Processing data for %s (interval: %dms)...\n", filename, interval_ms);

    // Parse ADC values
    long *raw_adc_values_long = NULL; // Store raw long values from parsing
    int raw_count = 0;
    char *content_copy = strdup(file_content); // strdup is standard on Linux
    if (content_copy == NULL) {
        perror("strdup failed");
        return;
    }

    char *line = strtok(content_copy, "\n");
    while (line != NULL) {
        if (strstr(line, "ADC:") != NULL) { // Look only for ADC lines
            long adc_val;
            // Scan for long integer
            if (sscanf(line, "ADC:%ld", &adc_val) == 1) {
                raw_count++;
                raw_adc_values_long = (long *)realloc(raw_adc_values_long, raw_count * sizeof(long));
                if (raw_adc_values_long == NULL) {
                    perror("realloc failed for raw_adc_values_long");
                    free(content_copy);
                    return;
                }
                raw_adc_values_long[raw_count - 1] = adc_val;
            }
        }
        line = strtok(NULL, "\n");
    }
    free(content_copy);

    if (raw_count == 0) {
        printf("No valid ADC values found in %s.\n", filename);
        return;
    }

    printf("Found %d ADC values.\n", raw_count);

    // Allocate memory for processing and output
    float32_t *raw_adc_values_f32 = (float32_t *)malloc(raw_count * sizeof(float32_t));
    float32_t *dc_removed_f32 = (float32_t *)malloc(raw_count * sizeof(float32_t));
    float32_t *filtered_f32 = (float32_t *)malloc(raw_count * sizeof(float32_t));
    double *raw_weights = (double *)malloc(raw_count * sizeof(double));
    double *filtered_weights = (double *)malloc(raw_count * sizeof(double));

    if (raw_adc_values_f32 == NULL || dc_removed_f32 == NULL || filtered_f32 == NULL || raw_weights == NULL || filtered_weights == NULL) {
        perror("Failed to allocate memory for DSP arrays");
        free(raw_adc_values_long);
        free(raw_adc_values_f32);
        free(dc_removed_f32);
        free(filtered_f32);
        free(raw_weights);
        free(filtered_weights);
        return;
    }

    // Convert raw long ADC values to float32_t for CMSIS-DSP operations
    for (int i = 0; i < raw_count; i++) {
        raw_adc_values_f32[i] = (float32_t)raw_adc_values_long[i];
    }
    
    // Calculate stable DC offset for the *entire* file for consistent re-offsetting
    float32_t stable_dc_offset = calculate_mean_f32(raw_adc_values_f32, raw_count);

    // Remove DC offset for FIR input
    for (int i = 0; i < raw_count; i++) {
        dc_removed_f32[i] = raw_adc_values_f32[i] - stable_dc_offset;
        raw_weights[i] = normalize_to_weight(raw_adc_values_long[i]); // Raw weights for output
    }

    // --- FIR Filtering using CMSIS-DSP ---
    printf("Applying FIR filter using CMSIS-DSP (Order: %d)...\n", FIR_NUM_TAPS);

    // Hardcoded FIR coefficients for a simple low-pass filter (similar to firwin output)
    // For a 50Hz sampling rate and 10Hz cutoff, these might be values from firwin(51, 10/25).
    // This is a simplified rectangular window (moving average) for demonstration with CMSIS-DSP.
    float32_t firCoeffs_f32[FIR_NUM_TAPS]; 
    for (int i = 0; i < FIR_NUM_TAPS; i++) {
        firCoeffs_f32[i] = 1.0f / (float32_t)FIR_NUM_TAPS; // Simple moving average
    }

    // Initialize FIR instance (raw_count is used as blockSize here, processing the whole file)
    arm_fir_init_f32(&S_fir, FIR_NUM_TAPS, firCoeffs_f32, firState_f32, raw_count);

    // Apply FIR filter using CMSIS-DSP
    arm_fir_f32(&S_fir, dc_removed_f32, filtered_f32, raw_count);
    
    // Re-add the stable DC offset and normalize to weights for filtered output
    for (int i = 0; i < raw_count; i++) {
        double re_offset_val = filtered_f32[i] + stable_dc_offset;
        filtered_weights[i] = normalize_to_weight((long)re_offset_val); // Cast to long for normalize_to_weight
    }
    printf("FIR filtering complete.\n");

    // --- FFT (Placeholder) ---
    printf("Note: FFT calculation is a placeholder in this C version.\n");


    // Output processed data to a file
    char output_filepath[256];
    snprintf(output_filepath, sizeof(output_filepath), "output_data/all_data_%s.txt", filename);

    struct stat st = {0};
    if (stat("output_data", &st) == -1) {
        mkdir("output_data", 0700); // Use mkdir on Linux
        printf("Created output folder: output_data\n");
    }

    FILE *output_file = fopen(output_filepath, "w");
    if (output_file == NULL) {
        perror("Error opening output file");
    } else {
        fprintf(output_file, "Raw Weights (first 10): [");
        for (int i = 0; i < fmin(10, raw_count); i++) {
            fprintf(output_file, "%.4f%s", raw_weights[i], (i == fmin(10, raw_count) - 1) ? "" : ", ");
        }
        fprintf(output_file, "]\n");
        fprintf(output_file, "Raw Weights (total %d samples): [", raw_count);
        for (int i = 0; i < raw_count; i++) {
            fprintf(output_file, "%.4f%s", raw_weights[i], (i == raw_count - 1) ? "" : ", "); 
        }
        fprintf(output_file, "]\n\n");

        fprintf(output_file, "Filtered Weights (first 10): [");
        for (int i = 0; i < fmin(10, raw_count); i++) {
            fprintf(output_file, "%.4f%s", filtered_weights[i], (i == fmin(10, raw_count) - 1) ? "" : ", ");
        }
        fprintf(output_file, "]\n");
        fprintf(output_file, "Filtered Weights (total %d samples): [", raw_count);
        for (int i = 0; i < raw_count; i++) {
            fprintf(output_file, "%.4f%s", filtered_weights[i], (i == raw_count - 1) ? "" : ", ");
        }
        fprintf(output_file, "]\n\n");

        fprintf(output_file, "FIR Coefficients: Moving Average (Order %d)\n\n", FIR_NUM_TAPS);
        fprintf(output_file, "FFT Frequencies: N/A (placeholder)\n\n");
        fprintf(output_file, "FFT Magnitudes: N/A (placeholder)\n\n");

        fclose(output_file);
        printf("Successfully wrote data to %s\n", output_filepath);
    }

    // Free allocated memory
    free(raw_adc_values_long);
    free(raw_adc_values_f32);
    free(dc_removed_f32);
    free(filtered_f32);
    free(raw_weights);
    free(filtered_weights);
}

// Normalizes an ADC value to a weight
double normalize_to_weight(long adc_value) {
    double data_in = (double)adc_value / (double)0x80000000;
    if (SCALE_CAL == 0) return 0.0; // Avoid division by zero
    return (data_in - ZERO_CAL) / SCALE_CAL;
}

// Calculates the mean of a float32_t array
float32_t calculate_mean_f32(float32_t *data, int count) {
    if (count == 0) return 0.0f;
    float32_t sum = 0.0f;
    for (int i = 0; i < count; i++) {
        sum += data[i];
    }
    return sum / (float32_t)count;
}