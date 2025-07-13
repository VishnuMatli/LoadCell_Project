// Define _WIN32_WINNT at the very top, before any other includes,
// to ensure it's defined for all Windows SDK headers.
#define _WIN32_WINNT 0x0600 // For Windows Vista and later, to ensure InetNtop is available

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>     // For uint32_t, uint64_t

// Windows-specific headers
#include <winsock2.h>   // For Winsock functions (SOCKET, WSADATA, etc.)
#include <ws2tcpip.h>   // For InetNtop (if available), sockaddr_in, etc.
#include <windows.h>    // For Sleep() function, MAKEWORD

// Needed for _mkdir on Windows, and stat
#include <sys/stat.h>
#include <direct.h>     // For _mkdir

// POSIX-like header for directory listing (since you have it)
#include <dirent.h> 

// Need to link with Ws2_32.lib (-lws2_32)

// Configuration
#define SERVER_IP "0.0.0.0" // Listen on all interfaces
#define SERVER_PORT 9999
#define BUFFER_SIZE 4096
#define DATA_FOLDER "adc_data"

// Define constants for length-prefixing (same as Python)
#define FILENAME_LENGTH_BYTES 4
#define FILE_CONTENT_LENGTH_BYTES 8
#define CONFIG_LENGTH_BYTES 4

// Function prototypes
// Note: SOCKET is a Windows-specific type for sockets
void send_length_prefixed_data(SOCKET sockfd, const char *filename, const char *file_content, size_t content_len, int is_file);
void handle_client(SOCKET client_sock);
void send_config(SOCKET client_sock, int interval_ms, const char *mode);
void send_file_by_path(SOCKET client_sock, const char *filepath);
void send_control_message(SOCKET client_sock, const char *message);

// Helper for htobe64 (host to big-endian 64-bit) for MinGW
#ifndef htobe64
static inline uint64_t htobe64_custom(uint64_t host_val) {
    return ((uint64_t)htonl((uint32_t)(host_val & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)(host_val >> 32));
}
#define htobe64 htobe64_custom
#endif


int main() {
    WSADATA wsaData;
    SOCKET server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_len = sizeof(client_addr);

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    // 1. Create socket
    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "Error creating socket: %d\n", WSAGetLastError());
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    // Allow immediate reuse of address (important for quick restarts)
    BOOL optval = TRUE;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval)) == SOCKET_ERROR) {
        fprintf(stderr, "setsockopt failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    // 2. Bind socket to address and port
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all available interfaces

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Error binding socket: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    // 3. Listen for incoming connections
    if (listen(server_sock, 5) == SOCKET_ERROR) {
        fprintf(stderr, "Error listening: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", SERVER_PORT);

    // Ensure data folder exists
    struct stat st = {0};
    if (stat(DATA_FOLDER, &st) == -1) {
        _mkdir(DATA_FOLDER); // Use _mkdir on Windows
        printf("Created data folder: %s\n", DATA_FOLDER);
    }

    while (1) {
        printf("Waiting for client connection...\n");
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock == INVALID_SOCKET) {
            fprintf(stderr, "Error accepting connection: %d\n", WSAGetLastError());
            continue;
        }

        // Use inet_ntoa for IP address to string conversion (most reliable on MinGW)
        char client_ip[INET_ADDRSTRLEN]; 
        char *ip_str_inet_ntoa = inet_ntoa(client_addr.sin_addr); 
        if (ip_str_inet_ntoa != NULL) {
            strncpy(client_ip, ip_str_inet_ntoa, sizeof(client_ip) - 1);
            client_ip[sizeof(client_ip) - 1] = '\0';
        } else {
            fprintf(stderr, "Error in inet_ntoa: %d\n", WSAGetLastError());
            strcpy(client_ip, "UNKNOWN"); 
        }
        
        printf("Connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        handle_client(client_sock); 
                                    
        printf("Client connection closed.\n");
        closesocket(client_sock); 
    }

    closesocket(server_sock); 
    WSACleanup();
    return 0;
}

// Handles a single client connection
void handle_client(SOCKET client_sock) {
    // In a real C application, you'd implement the mode selection logic here.
    // For this simplified version, we'll hardcode to "interval" mode for demonstration.
    const char *mode = "interval";
    int interval_ms = 20; // Default interval

    printf("Sending initial configuration...\n");
    send_config(client_sock, interval_ms, mode);

    DIR *d;
    struct dirent *dir;
    char filepath[256];
    int file_count = 0;

    d = opendir(DATA_FOLDER);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_REG && strstr(dir->d_name, ".txt") != NULL) { // Check for regular file and .txt extension
                file_count++;
                snprintf(filepath, sizeof(filepath), "%s/%s", DATA_FOLDER, dir->d_name);
                printf("Sending file: %s\n", dir->d_name);
                send_file_by_path(client_sock, filepath);
                Sleep(interval_ms); // Sleep for interval_ms milliseconds on Windows
            }
        }
        closedir(d);
    } else {
        perror("Could not open data directory");
        send_control_message(client_sock, "NO_FILES_IN_FOLDER");
        return;
    }

    if (file_count == 0) {
        printf("No .txt files found in %s.\n", DATA_FOLDER);
        send_control_message(client_sock, "NO_FILES_IN_FOLDER");
    } else {
        printf("Finished sending files.\n");
        send_control_message(client_sock, "END_OF_TRANSMISSION");
    }
}

// Sends configuration data (interval and mode)
void send_config(SOCKET client_sock, int interval_ms, const char *mode) {
    char config_str[BUFFER_SIZE];
    snprintf(config_str, sizeof(config_str), "INTERVAL:%d\\nMODE:%s\\n", interval_ms, mode);
    
    size_t config_len = strlen(config_str);
    uint32_t net_config_len = htonl(config_len); // Convert to network byte order

    if (send(client_sock, (const char*)&net_config_len, CONFIG_LENGTH_BYTES, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending config length: %d\n", WSAGetLastError());
        return;
    }
    if (send(client_sock, config_str, config_len, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending config data: %d\n", WSAGetLastError());
        return;
    }
    printf("Sent config: %s", config_str);
}


// Sends a file from a given path using length-prefixing
void send_file_by_path(SOCKET client_sock, const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        perror("Error opening file");
        send_control_message(client_sock, "FILE_NOT_FOUND"); // Send a specific error to client
        return;
    }

    // Get filename from path
    const char *filename = strrchr(filepath, '/'); 
    if (filename == NULL) {
        filename = strrchr(filepath, '\\'); 
    }
    if (filename == NULL) {
        filename = filepath; 
    } else {
        filename++; 
    }

    size_t filename_len = strlen(filename);
    uint32_t net_filename_len = htonl(filename_len);

    // Get file content length
    fseek(file, 0, SEEK_END);
    size_t file_content_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint64_t net_file_content_len = htobe64(file_content_len); // Use htobe64 for 8 bytes

    // 1. Send filename length
    if (send(client_sock, (const char*)&net_filename_len, FILENAME_LENGTH_BYTES, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending filename length: %d\n", WSAGetLastError());
        fclose(file);
        return;
    }

    // 2. Send filename
    if (send(client_sock, filename, filename_len, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending filename: %d\n", WSAGetLastError());
        fclose(file);
        return;
    }

    // 3. Send file content length
    if (send(client_sock, (const char*)&net_file_content_len, FILE_CONTENT_LENGTH_BYTES, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending file content length: %d\n", WSAGetLastError());
        fclose(file);
        return;
    }

    // 4. Send file content in chunks
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(client_sock, buffer, bytes_read, 0) == SOCKET_ERROR) {
            fprintf(stderr, "Error sending file content: %d\n", WSAGetLastError());
            fclose(file);
            return;
        }
    }

    // Corrected printf format for size_t using %lu (for unsigned long, most compatible)
    printf("Sent file: %s, Size: %lu bytes\n", filename, (unsigned long)file_content_len); 
    fclose(file);
}

// Sends a simple control message (like NO_FILE_FOUND)
void send_control_message(SOCKET client_sock, const char *message) {
    size_t msg_len = strlen(message);
    uint32_t net_msg_len = htonl(msg_len);
    uint64_t net_zero_content_len = htobe64(0); // Control messages have 0 content length

    if (send(client_sock, (const char*)&net_msg_len, FILENAME_LENGTH_BYTES, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending control message length: %d\n", WSAGetLastError());
        return;
    }
    if (send(client_sock, message, msg_len, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending control message: %d\n", WSAGetLastError());
        return;
    }
    if (send(client_sock, (const char*)&net_zero_content_len, FILE_CONTENT_LENGTH_BYTES, 0) == SOCKET_ERROR) {
        fprintf(stderr, "Error sending zero content length for control message: %d\n", WSAGetLastError());
        return;
    }
    printf("Sent control message: %s\n", message);
}