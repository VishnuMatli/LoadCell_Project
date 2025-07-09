#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> // For uint32_t, uint64_t

// Include for Windows specific functions and types
// _WIN32_WINNT is now expected to be defined by the compiler command line (-D_WIN32_WINNT=0x0600)
#include <winsock2.h> // For Winsock functions
#include <ws2tcpip.h> // For InetPton, etc. (though we'll use inet_addr/inet_ntoa for reliability)
#include <windows.h> // For Sleep() function

// Needed for _mkdir on Windows, and stat
#include <sys/stat.h>
#include <direct.h> // For _mkdir

#include <math.h> // For fmin

// Need to link with Ws2_32.lib (-lws2_32)

// Configuration
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999
#define BUFFER_SIZE 4096

// Define constants for length-prefixing (same as Python)
#define FILENAME_LENGTH_BYTES 4
#define FILE_CONTENT_LENGTH_BYTES 8
#define CONFIG_LENGTH_BYTES 4

// Calibration constants (from Python client)
#define ZERO_CAL 0.01823035255075
#define SCALE_CAL 0.00000451794631

// Function prototypes
ssize_t recv_all(SOCKET sockfd, void *buf, size_t len);
void process_data(const char *file_content, const char *filename, int interval_ms);
double normalize_to_weight(long adc_value);
double calculate_mean(double *data, int count);
void remove_dc_offset_simple(double *data, int count);


// Helper for be64toh (big-endian 64-bit to host)
// This custom implementation ensures compatibility across MinGW versions
#ifndef be64toh
static inline uint64_t be64toh_custom(uint64_t big_endian_val) {
    return ((uint64_t)ntohl((uint32_t)(big_endian_val & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)(big_endian_val >> 32));
}
#define be64toh be64toh_custom
#endif


int main() {
    WSADATA wsaData;
    SOCKET client_sock;
    struct sockaddr_in server_addr;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    // 1. Create socket
    client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCKET) {
        fprintf(stderr, "Error creating socket: %d\n", WSAGetLastError());
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    // 2. Connect to server
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    // Use inet_addr to convert IP string to binary form (more robust for MinGW)
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP); 
    if (server_addr.sin_addr.s_addr == INADDR_NONE && strcmp(SERVER_IP, "255.255.255.255") != 0) {
        fprintf(stderr, "Error: Invalid server IP address: %s\n", SERVER_IP);
        closesocket(client_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Error connecting to server: %d\n", WSAGetLastError());
        closesocket(client_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }
    printf("Connected to server.\n");

    // --- Phase 1: Receive Initial Configuration ---
    uint32_t net_config_len;
    if (recv_all(client_sock, &net_config_len, CONFIG_LENGTH_BYTES) <= 0) {
        printf("Server disconnected while receiving config length.\n");
        closesocket(client_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }
    size_t config_len = ntohl(net_config_len); // Convert from network byte order

    char *config_data = (char *)malloc(config_len + 1);
    if (config_data == NULL) {
        perror("Failed to allocate memory for config data");
        closesocket(client_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }
    if (recv_all(client_sock, config_data, config_len) <= 0) {
        printf("Server disconnected while receiving config data.\n");
        free(config_data);
        closesocket(client_sock);
        WSACleanup();
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

        // Corrected printf format for size_t
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

        // Process data (simplified in C)
        process_data(file_content, filename, interval_ms);

        free(filename);
        free(file_content);
    }

    printf("Connection closed.\n");
    closesocket(client_sock);
    WSACleanup(); // Clean up Winsock
    return 0;
}

// Helper function to ensure all bytes are received
ssize_t recv_all(SOCKET sockfd, void *buf, size_t len) {
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

// Processes the received data (simplified DSP and output)
void process_data(const char *file_content, const char *filename, int interval_ms) {
    printf("Processing data for %s (interval: %dms)...\n", filename, interval_ms);

    // Parse ADC values
    long *raw_adc_values = NULL;
    int raw_count = 0;
    char *content_copy = _strdup(file_content); // Use _strdup on Windows
    if (content_copy == NULL) {
        perror("_strdup failed");
        return;
    }

    char *line = strtok(content_copy, "\n");
    while (line != NULL) {
        if (strstr(line, "ADC:") != NULL) {
            long adc_val;
            if (sscanf(line, "ADC:%ld", &adc_val) == 1) {
                raw_count++;
                raw_adc_values = (long *)realloc(raw_adc_values, raw_count * sizeof(long));
                if (raw_adc_values == NULL) {
                    perror("realloc failed");
                    free(content_copy);
                    return;
                }
                raw_adc_values[raw_count - 1] = adc_val;
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

    // --- Simplified DSP Operations in C ---
    // For full DSP (FFT, FIR), you would integrate a C DSP library here (e.g., FFTW, or implement algorithms manually).
    // This example only shows basic DC offset removal and normalization.

    double *raw_weights = (double *)malloc(raw_count * sizeof(double));
    double *filtered_weights = (double *)malloc(raw_count * sizeof(double));
    double *dc_removed_values = (double *)malloc(raw_count * sizeof(double));

    if (raw_weights == NULL || filtered_weights == NULL || dc_removed_values == NULL) {
        perror("Failed to allocate memory for DSP arrays");
        free(raw_adc_values);
        free(raw_weights);
        free(filtered_weights);
        free(dc_removed_values);
        return;
    }

    // Convert raw ADC to double and store for DC removal
    for (int i = 0; i < raw_count; i++) {
        dc_removed_values[i] = (double)raw_adc_values[i];
    }

    // Simple DC offset removal
    double mean_val = calculate_mean(dc_removed_values, raw_count);
    for (int i = 0; i < raw_count; i++) {
        dc_removed_values[i] -= mean_val; // Now centered around zero
        raw_weights[i] = normalize_to_weight(raw_adc_values[i]); // Raw weights for comparison
    }

    // --- FIR Filtering (Placeholder) ---
    // In a real C application, you'd design and apply your FIR filter here.
    // For simplicity, we'll just use the raw_weights as "filtered" for now.
    // If you implement a real FIR, remember to apply it to dc_removed_values,
    // then re-add mean_val to the filtered_output, and then normalize_to_weight.
    for (int i = 0; i < raw_count; i++) {
        filtered_weights[i] = raw_weights[i]; // No actual filtering in this stub
    }
    printf("Note: FIR filtering is a placeholder in this C version.\n");


    // --- FFT (Placeholder) ---
    // For FFT, you would use a library like FFTW or implement a Cooley-Tukey algorithm.
    // This is just a placeholder to acknowledge the step.
    printf("Note: FFT calculation is a placeholder in this C version.\n");


    // Output processed data to a file
    char output_filepath[256];
    snprintf(output_filepath, sizeof(output_filepath), "output_data/all_data_%s.txt", filename);

    struct stat st = {0};
    if (stat("output_data", &st) == -1) {
        _mkdir("output_data"); // Use _mkdir on Windows
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

        fprintf(output_file, "FIR Coefficients: N/A (placeholder)\n\n");
        fprintf(output_file, "FFT Frequencies: N/A (placeholder)\n\n");
        fprintf(output_file, "FFT Magnitudes: N/A (placeholder)\n\n");

        fclose(output_file);
        printf("Successfully wrote data to %s\n", output_filepath);
    }

    free(raw_adc_values);
    free(raw_weights);
    free(filtered_weights);
    free(dc_removed_values);
}

// Normalizes an ADC value to a weight
double normalize_to_weight(long adc_value) {
    double data_in = (double)adc_value / (double)0x80000000;
    if (SCALE_CAL == 0) return 0.0; // Avoid division by zero
    return (data_in - ZERO_CAL) / SCALE_CAL;
}

// Calculates the mean of a double array
double calculate_mean(double *data, int count) {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += data[i];
    }
    return sum / count;
}