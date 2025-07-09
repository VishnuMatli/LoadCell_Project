#include <gtk/gtk.h> // This MUST be at the top for all GTK+ types and functions
#include <glib.h>    // Explicitly include for g_strdup_printf, g_usleep, g_path_get_basename, g_ascii_strdown

// Windows-specific networking headers (replaces sys/socket.h, netinet/in.h, arpa/inet.h)
#include <winsock2.h>
#include <ws2tcpip.h>

// Standard C Library includes
#include <pthread.h> // For threading (using pthreads-win32 or similar)
#include <stdio.h>   // For printf, fopen, fclose, etc.
#include <stdlib.h>  // For atoi, malloc, free, etc.
#include <string.h>  // For strcpy, strcmp, strstr, strlen
#include <dirent.h>  // For directory operations (opendir, readdir)
#include <sys/stat.h> // For mkdir
#include <sys/types.h> // For types like mode_t for mkdir
#include <errno.h>   // For error numbers
#include <stdbool.h> // For bool type

#ifdef _WIN32
#include <direct.h> // For _mkdir on Windows
#include <io.h>     // For access on Windows
#define access _access // Map POSIX access to Windows _access
#define F_OK 0 // Define F_OK for access on Windows
#pragma comment(lib, "Ws2_32.lib") // Link with Winsock library
#endif

// Explicitly define G_TRUE and G_FALSE if they are not picked up from glib.h
#ifndef G_TRUE
#define G_TRUE 1
#endif
#ifndef G_FALSE
#define G_FALSE 0
#endif


// Configuration
#define SERVER_IP "0.0.0.0" // Listen on all available interfaces
#define SERVER_PORT 9999
#define FOLDER "June23"    // Folder containing your .txt files (create this folder if it doesn't exist)
#define BUFFER_SIZE 4096   // Buffer size for reading/sending file data

// --- Network Protocol Configuration ---
// These constants must match the client's constants for binary data framing
#define FILENAME_LENGTH_BYTES 4
#define FILE_CONTENT_LENGTH_BYTES 8
#define CONFIG_LENGTH_BYTES 4

// Custom 64-bit host-to-network byte swap (needed for FILE_CONTENT_LENGTH_BYTES)
#ifdef _MSC_VER // Microsoft Visual C++
#include <intrin.h>
#define htonll_custom(x) _byteswap_uint64(x)
#else // Assume GCC/Clang with __builtin_bswap64 for cross-platform or use manual if not available
// This manual implementation handles byte order correctly for 64-bit
uint64_t htonll_custom(uint64_t val) {
    return (((uint64_t)htonl((uint32_t)(val & 0xFFFFFFFF))) << 32) | htonl((uint32_t)(val >> 32));
}
#endif


// Forward declaration for the server thread function
void *send_files_thread_func(void *arg);

// Global UI Elements and Server State
// Structure to hold all application widgets and shared state
typedef struct {
    GtkWindow *main_window;
    GtkLabel *status_label;
    GtkDrawingArea *status_indicator;
    GdkRGBA status_circle_color; // Color for the status circle
    GtkLabel *overall_status_label;
    GtkRadioButton *radio_interval;
    GtkRadioButton *radio_freq;
    GtkRadioButton *radio_select_file;
    GtkComboBoxText *interval_menu;
    GtkEntry *freq_entry;
    GtkButton *select_file_button;
    GtkLabel *selected_file_label;
    GtkButton *start_button;
    GtkLabel *label_interval_widget; // Added for explicit access
    GtkLabel *label_freq_widget;     // Added for explicit access

    char *selected_file_path; // Dynamically allocated path of the selected file
    bool server_running; // Flag to indicate if the server thread is active
    SOCKET server_socket_fd; // Changed to SOCKET type
    SOCKET client_socket_fd; // Changed to SOCKET type
    pthread_t server_thread_id; // ID of the server thread
    char current_mode[20]; // Stores "interval", "freq", "select_file"
    bool terminate_server_thread; // Flag to signal thread termination
    WSADATA wsaData; // For Winsock initialization on Windows (used in main)
} AppWidgets;

AppWidgets *app_widgets; // Global pointer to the application widgets and state

// --- UI Update Helper Functions (Thread-safe) ---
// These functions are called from the worker thread and marshal updates to the main GTK+ thread.

/**
 * @brief Updates the main status label and the status indicator color.
 * @param data A dynamically allocated string containing the status text, followed by a null terminator,
 * and then the hex color string (e.g., "#RRGGBB").
 * @return G_SOURCE_REMOVE to indicate the source should be removed after execution.
 */
static gboolean update_overall_status_ui(gpointer data) {
    char *combined_text = (char *)data;
    char *text = combined_text;
    char *color_str = text + strlen(text) + 1; // Get color string after null terminator

    gtk_label_set_text(app_widgets->overall_status_label, text);
    gdk_rgba_parse(&app_widgets->status_circle_color, color_str);
    gtk_widget_queue_draw(GTK_WIDGET(app_widgets->status_indicator)); // Redraw status indicator
    g_free(combined_text); // Free the allocated string
    return G_SOURCE_REMOVE;
}

/**
 * @brief Updates the small server status label next to the indicator.
 * @param data A dynamically allocated string for the status text.
 * @return G_SOURCE_REMOVE.
 */
static gboolean update_server_status_label_ui(gpointer data) {
    char *text = (char *)data;
    gtk_label_set_text(app_widgets->status_label, text);
    g_free(text);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Updates the label showing the selected file name.
 * @param data A dynamically allocated string for the file name.
 * @return G_SOURCE_REMOVE.
 */
static gboolean update_selected_file_label_ui(gpointer data) {
    char *text = (char *)data;
    gtk_label_set_text(app_widgets->selected_file_label, text);
    g_free(text);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Sets the sensitivity (enabled/disabled state) of a GtkWidget.
 * @param data A pointer to the GtkWidget to modify.
 * @return G_SOURCE_REMOVE.
 */
static gboolean set_widget_sensitive_ui(gpointer data) {
    GtkWidget *widget = (GtkWidget *)((gpointer *)data)[0];
    bool sensitive = (bool)((gsize)((gpointer *)data)[1]);
    gtk_widget_set_sensitive(widget, sensitive);
    g_free(data);
    return G_SOURCE_REMOVE;
}


/**
 * @brief Helper to safely update the overall status label and indicator color from any thread.
 * @param text The status message.
 * @param hex_color The hex color string (e.g., "#00FF00").
 */
void gui_update_overall_status(const char *text, const char *hex_color) {
    // Combine text and color string, separated by a null byte
    char *combined_text = g_strdup_printf("%s%c%s", text, '\0', hex_color);
    g_idle_add(update_overall_status_ui, combined_text);
}

/**
 * @brief Helper to safely update the small server status label from any thread.
 * @param text The status message.
 */
void gui_update_server_status_label(const char *text) {
    g_idle_add(update_server_status_label_ui, g_strdup(text));
}

/**
 * @brief Helper to safely update the selected file label from any thread.
 * @param text The text to display for the selected file.
 */
void gui_update_selected_file_label(const char *text) {
    g_idle_add(update_selected_file_label_ui, g_strdup(text));
}

/**
 * @brief Helper to safely set the sensitivity of a widget from any thread.
 * @param widget The GtkWidget to modify.
 * @param sensitive True to enable, false to disable.
 */
void gui_set_widget_sensitive(GtkWidget *widget, bool sensitive) {
    gpointer *data = g_new(gpointer, 2);
    data[0] = widget;
    data[1] = (gpointer)(gsize)sensitive;
    g_idle_add(set_widget_sensitive_ui, data);
}

// --- GTK+ Styles ---
/**
 * @brief Applies custom CSS styles to the GTK+ application.
 * This attempts to replicate the futuristic theme from the Python Tkinter app.
 */
static void apply_styles(void) {
    GtkCssProvider *provider;
    GdkDisplay *display;
    GdkScreen *screen;

    provider = gtk_css_provider_new();
    display = gdk_display_get_default();
    screen = gdk_display_get_default_screen(display);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // GTK+ CSS is used to style widgets. Selectors target widget names or classes.
    const char *css_data =
        "window {"
        "   background-color: #2C3E50;"
        "}"
        "label {"
        "   color: #ecf0f1;" // Default text color
        "   background-color: transparent;"
        "}"
        "GtkLabel#title_label {"
        "   font-family: \"Helvetica\";"
        "   font-size: 24pt;"
        "   font-weight: bold;"
        "   color: #00FFFF;"
        "}"
        "GtkFrame {" // For GtkLabelFrame equivalent
        "   background-color: #2C3E50;"
        "   border: 1px solid #00FFFF;"
        "   border-radius: 5px;"
        "   padding: 10px;"
        "}"
        "GtkFrame > GtkLabel {" // Label of the GtkLabelFrame
        "   font-family: \"Helvetica\";"
        "   font-size: 12pt;"
        "   font-weight: bold;"
        "   color: #00FFFF;"
        "   background-color: #2C3E50;" // Background to hide border underneath label
        "   margin-left: 5px; margin-right: 5px;"
        "}"
        "radiobutton {"
        "   font-family: \"Helvetica\";"
        "   font-size: 11pt;"
        "   color: #ecf0f1;"
        "   background-color: #2C3E50;"
        "}"
        "radiobutton:checked {"
        "   color: #00FFFF;"
        "}"
        "radiobutton indicator {"
        "   background-color: #00FFFF;" // Inner circle color when checked
        "   border-radius: 50%;"
        "   border: 1px solid #00FFFF;"
        "}"
        "radiobutton:checked indicator {"
        "   background-color: #00FFFF;"
        "}"
        "entry {"
        "   font-family: \"Helvetica\";"
        "   font-size: 12pt;"
        "   padding: 5px;"
        "   background-color: #34495E;"
        "   color: white;"
        "   caret-color: #00FFFF;"
        "   border-width: 1px;"
        "   border-style: solid;"
        "   border-color: #34495E;"
        "   border-radius: 3px;"
        "}"
        "combobox {"
        "   font-family: \"Helvetica\";"
        "   font-size: 12pt;"
        "   padding: 5px;"
        "}"
        "combobox entry {" // Style the entry part of the combobox
        "   background-color: #34495E;"
        "   color: white;"
        "   caret-color: #00FFFF;"
        "   border-width: 1px;"
        "   border-style: solid;"
        "   border-color: #34495E;"
        "   border-radius: 3px;"
        "}"
        "button {"
        "   border-radius: 5px;"
        "   padding: 8px 15px;"
        "   border: none;"
        "   font-family: \"Helvetica\";"
        "   font-weight: bold;"
        "}"
        "button#primary_button {" // Style for "Start Sending Data"
        "   font-size: 14pt;"
        "   background-color: #00FFFF;"
        "   color: black;"
        "}"
        "button#primary_button:hover {"
        "   background-color: #00b894;"
        "   color: white;"
        "}"
        "button#accent_button {" // Style for "Select File"
        "   font-size: 11pt;"
        "   background-color: #00b894;"
        "   color: white;"
        "}"
        "button#accent_button:hover {"
        "   background-color: #008c70;"
        "}"
        "button:disabled {"
        "   background-color: #555555;"
        "   color: #aaaaaa;"
        "}"
        "label#status_label_small {"
        "   font-family: \"Helvetica\";"
        "   font-size: 11pt;"
        "   color: #D3D3D3;"
        "}"
        "label#overall_status_label_big {"
        "   font-family: \"Helvetica\";"
        "   font-size: 12pt;"
        "   color: #00FF00;" // Default success color
        "}"
        "label#selected_file_display_label {"
        "   font-family: \"Helvetica\";"
        "   font-size: 10pt;"
        "   font-style: italic;"
        "   color: #A9A9A9;"
        "}"
        "label.disabled_label {" // New CSS class for disabled labels
        "   color: #808080;"
        "}"
        ;

    gtk_css_provider_load_from_data(provider, css_data, -1, NULL);
    g_object_unref(provider);
}

// --- GTK+ Callbacks ---

/**
 * @brief Callback function for drawing the status indicator circle.
 * @param widget The GtkDrawingArea widget.
 * @param cr The Cairo context for drawing.
 * @param data User data (not used here).
 * @return G_TRUE to indicate the drawing has been handled.
 */
gboolean draw_status_indicator(GtkWidget *widget, cairo_t *cr, gpointer data) {
    GdkRGBA *color = &app_widgets->status_circle_color;

    cairo_set_source_rgb(cr, color->red, color->green, color->blue);
    cairo_arc(cr, 7.5, 7.5, 5, 0, 2 * G_PI); // Center (7.5, 7.5), radius 5
    cairo_fill(cr);

    // Optional: Add a white border
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // White
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, 7.5, 7.5, 5, 0, 2 * G_PI);
    cairo_stroke(cr);

    return G_TRUE;
}

/**
 * @brief Updates the sensitivity of input controls based on the selected mode.
 */
static void update_mode_controls(void) {
    // Correctly determine the current mode based on active radio button
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app_widgets->radio_interval))) {
        strcpy(app_widgets->current_mode, "interval");
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app_widgets->radio_freq))) {
        strcpy(app_widgets->current_mode, "freq");
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app_widgets->radio_select_file))) {
        strcpy(app_widgets->current_mode, "select_file");
    }

    // Assign CSS classes to the labels themselves, now accessible via app_widgets
    if (strcmp(app_widgets->current_mode, "interval") == 0) {
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->interval_menu), TRUE);
        gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->label_interval_widget)), "disabled_label");
        
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->freq_entry), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->label_freq_widget)), "disabled_label");
        
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->select_file_button), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->selected_file_label)), "disabled_label");

    } else if (strcmp(app_widgets->current_mode, "freq") == 0) {
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->interval_menu), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->label_interval_widget)), "disabled_label");
        
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->freq_entry), TRUE);
        gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->label_freq_widget)), "disabled_label");
        
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->select_file_button), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->selected_file_label)), "disabled_label");

    } else if (strcmp(app_widgets->current_mode, "select_file") == 0) {
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->interval_menu), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->label_interval_widget)), "disabled_label");
        
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->freq_entry), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->label_freq_widget)), "disabled_label");
        
        gui_set_widget_sensitive(GTK_WIDGET(app_widgets->select_file_button), TRUE);
        gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(app_widgets->selected_file_label)), "disabled_label"); // Enable default style
    }

    // Reset selected file if mode changes from "select_file"
    if (strcmp(app_widgets->current_mode, "select_file") != 0 && app_widgets->selected_file_path != NULL) {
        g_free(app_widgets->selected_file_path);
        app_widgets->selected_file_path = NULL;
        gui_update_selected_file_label("No file selected"); // Update UI via helper
    }
}

/**
 * @brief Callback for the "Select File" button. Opens a file chooser dialog.
 * @param button The GtkButton that was clicked.
 * @param user_data User data (app_widgets pointer).
 */
static void select_file_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *dialog;
    dialog = gtk_file_chooser_dialog_new("Select a File",
                                         app_widgets->main_window,
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "Cancel", GTK_RESPONSE_CANCEL,
                                         "Open", GTK_RESPONSE_ACCEPT,
                                         NULL);

    // Set initial directory
    char initial_dir[PATH_MAX]; 
    g_snprintf(initial_dir, sizeof(initial_dir), "%s%s%s", g_get_current_dir(), G_DIR_SEPARATOR_S, FOLDER);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), initial_dir);

    // Add filter for text files
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Text Files (*.txt)");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (app_widgets->selected_file_path) {
            g_free(app_widgets->selected_file_path);
        }
        app_widgets->selected_file_path = filename; // filename is already dynamically allocated by GTK+

        char *base_filename = g_path_get_basename(filename);
        char label_text[256];
        g_snprintf(label_text, sizeof(label_text), "Selected: %s", base_filename);
        gui_update_selected_file_label(label_text); // Use helper for UI update
        g_free(base_filename);

        gui_update_overall_status("File selected. Ready to send.", "#00FF00");
    } else {
        if (app_widgets->selected_file_path) {
            g_free(app_widgets->selected_file_path);
            app_widgets->selected_file_path = NULL;
        }
        gui_update_selected_file_label("No file selected"); // Use helper for UI update
        gui_update_overall_status("No file selected.", "orange");
    }
    gtk_widget_destroy(dialog);
}

/**
 * @brief Callback for the "Start Sending Data" button. Initiates the server thread.
 * @param button The GtkButton that was clicked.
 * @param user_data User data (app_widgets pointer).
 */
static void start_sending_clicked(GtkButton *button, gpointer user_data) {
    if (app_widgets->server_running) {
        gui_update_overall_status("Server already running!", "orange");
        return;
    }

    // Attempt to join any previous, potentially lingering thread (though daemonized)
    if (app_widgets->server_thread_id != 0 && pthread_join(app_widgets->server_thread_id, NULL) != 0) {
        fprintf(stderr, "Warning: Could not join previous server thread.\n");
    }
    app_widgets->server_thread_id = 0; // Reset ID after attempt to join

    // Close any previous sockets if they were left open
    if (app_widgets->server_socket_fd != INVALID_SOCKET) { // Use INVALID_SOCKET
        closesocket(app_widgets->server_socket_fd);
        app_widgets->server_socket_fd = INVALID_SOCKET;
    }
    if (app_widgets->client_socket_fd != INVALID_SOCKET) { // Use INVALID_SOCKET
        closesocket(app_widgets->client_socket_fd);
        app_widgets->client_socket_fd = INVALID_SOCKET;
    }

    gui_update_overall_status("Starting server...", "#00FFFF");
    gui_update_server_status_label("Server Starting...");

    app_widgets->server_running = true;
    app_widgets->terminate_server_thread = false; // Reset termination flag

    // Create the server thread
    int ret = pthread_create(&app_widgets->server_thread_id, NULL, send_files_thread_func, NULL);
    if (ret != 0) {
        gui_update_overall_status(g_strdup_printf("Failed to create server thread: %s", strerror(ret)), "red");
        gui_update_server_status_label("Server Error");
        app_widgets->server_running = false;
        app_widgets->server_thread_id = 0; // Reset thread ID on failure
    }
    // else { thread will update status when it starts listening/connecting }
}

/**
 * @brief Callback for when the main window is closed. Cleans up resources.
 * @param widget The GtkWindow that is being destroyed.
 * @param event The event (not used).
 * @param user_data User data (app_widgets pointer).
 * @return G_FALSE to allow the window to be destroyed.
 */
static gboolean on_main_window_destroy(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    // Signal the server thread to terminate
    if (app_widgets->server_running && app_widgets->server_thread_id != 0) {
        app_widgets->terminate_server_thread = true;
        // Close sockets to unblock accept() or send() if thread is waiting
        if (app_widgets->server_socket_fd != INVALID_SOCKET) {
            shutdown(app_widgets->server_socket_fd, SD_BOTH); // Use SD_BOTH for Winsock
            closesocket(app_widgets->server_socket_fd);
            app_widgets->server_socket_fd = INVALID_SOCKET;
        }
        if (app_widgets->client_socket_fd != INVALID_SOCKET) {
            shutdown(app_widgets->client_socket_fd, SD_BOTH);
            closesocket(app_widgets->client_socket_fd);
            app_widgets->client_socket_fd = INVALID_SOCKET;
        }
        pthread_join(app_widgets->server_thread_id, NULL); // Wait for the thread to finish
        app_widgets->server_thread_id = 0;
    }

    if (app_widgets->selected_file_path) {
        g_free(app_widgets->selected_file_path);
        app_widgets->selected_file_path = NULL;
    }
    // No explicit pthread_mutex_destroy needed if it's not used, or if GTK is single-threaded by default
    WSACleanup(); // Clean up Winsock DLL
    g_free(app_widgets); // Free the global app_widgets structure
    gtk_main_quit(); // Quit the GTK+ main loop
    return G_FALSE;
}

/**
 * @brief Sends a single file over the provided socket connection using the defined protocol.
 * @param conn_fd The client socket file descriptor.
 * @param filepath The full path to the file to send.
 * @param file_basename The base name of the file (caller responsible for freeing if dynamically obtained)
 */
void send_file(SOCKET conn_fd, const char *filepath, const char* file_basename) { // Changed conn_fd to SOCKET
    FILE *file = NULL;
    long file_size = 0;
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    const char *basename_to_use = file_basename ? file_basename : g_path_get_basename(filepath); // Use provided basename or get from path

    if (app_widgets->terminate_server_thread) {
        gui_update_overall_status(g_strdup_printf("Server stopping, skipping: %s", basename_to_use), "orange");
        if (!file_basename) g_free((gpointer)basename_to_use); // Free only if dynamically obtained
        return;
    }

    // --- Send filename length and filename ---
    uint32_t filename_length = strlen(basename_to_use);
    uint32_t filename_length_net = htonl(filename_length);
    if (send(conn_fd, (const char*)&filename_length_net, FILENAME_LENGTH_BYTES, 0) < 0) { // Added cast
        perror("send filename length failed");
        gui_update_overall_status(g_strdup_printf("Error sending filename length for %s", basename_to_use), "red");
        if (!file_basename) g_free((gpointer)basename_to_use);
        return;
    }
    if (send(conn_fd, basename_to_use, filename_length, 0) < 0) {
        perror("send filename failed");
        gui_update_overall_status(g_strdup_printf("Error sending filename %s", basename_to_use), "red");
        if (!file_basename) g_free((gpointer)basename_to_use);
        return;
    }
    
    file = fopen(filepath, "rb");
    if (!file) {
        gui_update_overall_status(g_strdup_printf("Error: File not found: %s", basename_to_use), "red");
        fprintf(stderr, "Error: File not found at %s: %s\n", filepath, strerror(errno));
        // --- Send 0 content length to indicate no content follows ---
        uint64_t file_content_length_net = htonll_custom(0);
        send(conn_fd, (const char*)&file_content_length_net, FILE_CONTENT_LENGTH_BYTES, 0); // Added cast
        if (!file_basename) g_free((gpointer)basename_to_use);
        return;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // --- Send file content length ---
    uint64_t file_content_length = file_size;
    uint64_t file_content_length_net = htonll_custom(file_content_length);
    if (send(conn_fd, (const char*)&file_content_length_net, FILE_CONTENT_LENGTH_BYTES, 0) < 0) { // Added cast
        perror("send file content length failed");
        gui_update_overall_status(g_strdup_printf("Error sending content length for %s", basename_to_use), "red");
        fclose(file);
        if (!file_basename) g_free((gpointer)basename_to_use);
        return;
    }
    
    // --- Send file data ---
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (app_widgets->terminate_server_thread) { // Check termination flag during send
            gui_update_overall_status("Server stopping during file send.", "orange");
            break;
        }
        if (send(conn_fd, buffer, bytes_read, 0) < 0) {
            perror("send data failed");
            gui_update_overall_status(g_strdup_printf("Error sending data for %s: %s", basename_to_use, strerror(errno)), "red");
            break; // Exit loop on send error
        }
    }

    fclose(file);
    if (!app_widgets->terminate_server_thread) {
        gui_update_overall_status(g_strdup_printf("Sent: %s", basename_to_use), "#00FF00");
        printf("Sent file: %s\n", basename_to_use);
    }
    if (!file_basename) g_free((gpointer)basename_to_use); // Free only if dynamically obtained
}

/**
 * @brief The main function for the server thread. Handles socket listening and file sending.
 * @param arg Not used.
 * @return NULL on thread exit.
 */
void *send_files_thread_func(void *arg) {
    SOCKET listen_fd = INVALID_SOCKET; // Changed to SOCKET type
    SOCKET conn_fd = INVALID_SOCKET;   // Changed to SOCKET type
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Create socket
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd == INVALID_SOCKET) {
        fprintf(stderr, "socket creation failed: %d\n", WSAGetLastError());
        gui_update_overall_status("Server Error: Socket creation failed.", "red");
        gui_update_server_status_label("Server Error");
        goto cleanup;
    }
    app_widgets->server_socket_fd = listen_fd; // Store fd in app_widgets

    // Allow immediate reuse of address
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) == SOCKET_ERROR) {
        fprintf(stderr, "setsockopt failed: %d\n", WSAGetLastError());
        gui_update_overall_status("Server Error: setsockopt failed.", "red");
        goto cleanup;
    }

    // Prepare server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    // Bind socket
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "socket bind failed: %d\n", WSAGetLastError());
        gui_update_overall_status(g_strdup_printf("Server Error: Bind failed on %s:%d (Is port in use?)", SERVER_IP, SERVER_PORT), "red");
        gui_update_server_status_label("Server Error");
        goto cleanup;
    }

    // Listen for incoming connections
    if (listen(listen_fd, 1) == SOCKET_ERROR) {
        fprintf(stderr, "socket listen failed: %d\n", WSAGetLastError());
        gui_update_overall_status("Server Error: Listen failed.", "red");
        gui_update_server_status_label("Server Error");
        goto cleanup;
    }

    gui_update_overall_status(g_strdup_printf("Waiting for client connection on %s:%d...", SERVER_IP, SERVER_PORT), "yellow");
    gui_update_server_status_label("Waiting for Client...");
    printf("Server listening on %s:%d\n", SERVER_IP, SERVER_PORT);

    // Accept a client connection
    conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (conn_fd == INVALID_SOCKET) {
        int wsa_error = WSAGetLastError();
        if (wsa_error == WSAEINTR || wsa_error == WSAENOTSOCK) { // Socket was closed/interrupted by shutdown
            printf("Accept interrupted or socket closed, server stopping.\n");
            gui_update_overall_status("Server stopped.", "orange");
            gui_update_server_status_label("Server Offline");
        } else {
            fprintf(stderr, "socket accept failed: %d\n", wsa_error);
            gui_update_overall_status("Server Error: Accept failed.", "red");
            gui_update_server_status_label("Server Error");
        }
        goto cleanup;
    }
    app_widgets->client_socket_fd = conn_fd; // Store client fd

    gui_update_overall_status(g_strdup_printf("Connected to %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port)), "#00FF00");
    gui_update_server_status_label("Client Connected");
    printf("Client connected from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // Get selected mode (read-only access, no mutex needed if done once at start)
    char mode_selected[20];
    strcpy(mode_selected, app_widgets->current_mode); // Assume current_mode is set before thread starts


    // --- Send Configuration to Client ---
    char config_msg[256];
    int chosen_interval_ms = 100; // NEW DEFAULT: Send 100ms interval for slower plotting

    if (strcmp(mode_selected, "interval") == 0) {
        const char *interval_str = gtk_combo_box_text_get_active_text(app_widgets->interval_menu);
        if (interval_str) {
            int parsed_interval = atoi(interval_str);
            if (parsed_interval > 0) { // Ensure positive interval
                chosen_interval_ms = parsed_interval;
            } else {
                chosen_interval_ms = 100; // Fallback to 100ms if parsing yields 0 or negative
            }
        }
    } else if (strcmp(mode_selected, "freq") == 0) {
        const char *freq_str = gtk_entry_get_text(app_widgets->freq_entry);
        g_snprintf(config_msg, sizeof(config_msg), "MODE:FREQ,FREQ_HZ:%s", freq_str); // This will overwrite config_msg
        chosen_interval_ms = 100; // Maintain 100ms for non-interval modes
    }
    // If mode is "select_file", it will also use the default chosen_interval_ms (100ms)
    
    // Construct the actual message to send. If it's a specific mode message, use that, else use INTERVAL
    if (strcmp(mode_selected, "interval") == 0) {
        g_snprintf(config_msg, sizeof(config_msg), "INTERVAL:%d", chosen_interval_ms);
    }
    // For other modes, the config_msg might have been set to MODE:FREQ, etc.
    // The client currently only parses INTERVAL, so we just ensure config_msg contains a valid interval.
    // A more robust protocol might send MODE and INTERVAL separately, or a structured config.

    uint32_t config_len = strlen(config_msg);
    uint32_t config_len_net = htonl(config_len);
    if (send(conn_fd, (const char*)&config_len_net, CONFIG_LENGTH_BYTES, 0) < 0) { // Added cast
        perror("send config length failed"); gui_update_overall_status("Error sending config length.", "red"); goto cleanup;
    }
    if (send(conn_fd, config_msg, config_len, 0) < 0) {
        perror("send config data failed"); gui_update_overall_status("Error sending config data.", "red"); goto cleanup;
    }
    printf("Sent config: %s (Chosen interval for client: %d ms)\n", config_msg, chosen_interval_ms);


    // --- File Sending Logic based on Mode ---
    if (strcmp(mode_selected, "interval") == 0) {
        // Use chosen_interval_ms for sleeping
        long sleep_us = (long)chosen_interval_ms * 1000; 

        DIR *d;
        struct dirent *dir;
        char full_path[PATH_MAX];
        char **file_list = NULL;
        int file_count = 0;
        int capacity = 10; // Initial capacity for file_list

        file_list = g_new(char *, capacity); // Use GLib's allocator
        if (!file_list) { perror("g_new failed"); goto cleanup_file_list; }

        d = opendir(FOLDER);
        if (d) {
            while ((dir = readdir(d)) != NULL) {
                if (strstr(dir->d_name, ".txt") != NULL) { // Check if it's a .txt file
                    if (file_count >= capacity) {
                        capacity *= 2;
                        file_list = g_realloc(file_list, capacity * sizeof(char *));
                        if (!file_list) { perror("g_realloc failed"); closedir(d); goto cleanup_file_list; }
                    }
                    file_list[file_count] = g_strdup(dir->d_name);
                    file_count++;
                }
            }
            closedir(d);
        } else {
            gui_update_overall_status(g_strdup_printf("Error: Could not open directory %s", FOLDER), "red");
            // Send special filename to client if folder not found
            send_file(conn_fd, "", "NO_FILES_IN_FOLDER");
            goto cleanup;
        }

        // Sort files alphabetically (basic string comparison)
        for (int i = 0; i < file_count - 1; i++) {
            for (int j = i + 1; j < file_count; j++) {
                if (strcmp(file_list[i], file_list[j]) > 0) {
                    char *temp = file_list[i];
                    file_list[i] = file_list[j];
                    file_list[j] = temp;
                }
            }
        }

        for (int i = 0; i < file_count; i++) {
            if (app_widgets->terminate_server_thread) break; // Check termination flag
            g_snprintf(full_path, sizeof(full_path), "%s%s%s", FOLDER, G_DIR_SEPARATOR_S, file_list[i]); // Use G_DIR_SEPARATOR_S
            if (access(full_path, F_OK) == 0) { // Check if file exists
                send_file(conn_fd, full_path, NULL); // Let send_file get basename
                if (app_widgets->terminate_server_thread) break;
                g_usleep(sleep_us); // Sleep for the interval
            } else {
                gui_update_overall_status(g_strdup_printf("File not found during interval send: %s", file_list[i]), "red");
                fprintf(stderr, "File not found during interval send: %s\n", full_path);
                send_file(conn_fd, "", g_strdup_printf("NO_FILE_FOUND:%s", file_list[i])); // Send specific error filename
            }
            g_free(file_list[i]); // Free individual filename string
        }
        cleanup_file_list:
        g_free(file_list); // Free the list array itself

    } else if (strcmp(mode_selected, "freq") == 0) {
        const char *freq_str = gtk_entry_get_text(app_widgets->freq_entry);
        char search_pattern[64];
        g_snprintf(search_pattern, sizeof(search_pattern), "hz%s", freq_str); // Example: "50hz.txt"

        DIR *d;
        struct dirent *dir;
        char found_file_path[PATH_MAX] = {0};
        char found_file_name[256] = {0};

        d = opendir(FOLDER);
        if (d) {
            while ((dir = readdir(d)) != NULL) {
                char *lower_d_name = g_ascii_strdown(dir->d_name, -1);
                // Check if it's a .txt file AND contains the freq pattern
                if (strstr(dir->d_name, ".txt") != NULL && strstr(lower_d_name, search_pattern) != NULL) {
                    g_snprintf(found_file_path, sizeof(found_file_path), "%s%s%s", FOLDER, G_DIR_SEPARATOR_S, dir->d_name);
                    strncpy(found_file_name, dir->d_name, sizeof(found_file_name) - 1);
                    found_file_name[sizeof(found_file_name) - 1] = '\0';
                    g_free(lower_d_name);
                    break;
                }
                g_free(lower_d_name);
            }
            closedir(d);
        }

        if (strlen(found_file_path) > 0) {
            send_file(conn_fd, found_file_path, NULL); // Use send_file to handle basename
        } else {
            gui_update_overall_status(g_strdup_printf("No file found for %s Hz", freq_str), "orange");
            fprintf(stderr, "No file found for %s Hz\n", freq_str);
            char specific_error_name[256];
            g_snprintf(specific_error_name, sizeof(specific_error_name), "NO_FILE_FOUND:%sHz", freq_str);
            send_file(conn_fd, "", specific_error_name); // Send specific error filename with no content
        }
    } else if (strcmp(mode_selected, "select_file") == 0) {
        // Here, we don't need a mutex because app_widgets->selected_file_path is only set/cleared on GUI thread.
        // The server thread reads it *after* button click which is on GUI thread.
        if (app_widgets->selected_file_path && access(app_widgets->selected_file_path, F_OK) == 0) {
            send_file(conn_fd, app_widgets->selected_file_path, NULL);
        } else {
            gui_update_overall_status("No file selected or file does not exist!", "red");
            fprintf(stderr, "No file selected or file does not exist!\n");
            send_file(conn_fd, "", "NO_FILE_SELECTED"); // Send specific error filename with no content
        }
    }

    // --- Send END_OF_TRANSMISSION signal ---
    // This tells the client that no more files are coming in this session.
    if (!app_widgets->terminate_server_thread) { // Only send if not explicitly terminating
        printf("Sending END_OF_TRANSMISSION signal.\n");
        send_file(conn_fd, "", "END_OF_TRANSMISSION"); // Use send_file for EOT
    }

cleanup:
    if (conn_fd != INVALID_SOCKET) { closesocket(conn_fd); app_widgets->client_socket_fd = INVALID_SOCKET; }
    if (listen_fd != INVALID_SOCKET) { closesocket(listen_fd); app_widgets->server_socket_fd = INVALID_SOCKET; }
    
    // Only update status if the thread was not explicitly asked to terminate
    if (!app_widgets->terminate_server_thread) {
        gui_update_overall_status("Finished sending files. Connection closed.", "#00FF00");
        gui_update_server_status_label("Server Offline");
        printf("Finished sending files. Connection closed.\n");
    } else {
        gui_update_overall_status("Server shut down.", "orange");
        gui_update_server_status_label("Server Offline");
        printf("Server thread terminated.\n");
    }
    app_widgets->server_running = false;
    app_widgets->server_thread_id = 0; // Reset thread ID
    return NULL;
}


// --- Main Application Setup ---

/**
 * @brief Initializes and sets up the main GTK+ application window and widgets.
 * @param app The GtkApplication instance.
 * @param user_data User data (not used here).
 */
static void activate(GtkApplication *app, gpointer user_data) {
    // Allocate and initialize app_widgets structure
    app_widgets = g_new0(AppWidgets, 1);
    app_widgets->server_socket_fd = INVALID_SOCKET; // Initialize to INVALID_SOCKET
    app_widgets->client_socket_fd = INVALID_SOCKET; // Initialize to INVALID_SOCKET
    app_widgets->server_thread_id = 0;
    app_widgets->server_running = false;
    app_widgets->selected_file_path = NULL;
    app_widgets->terminate_server_thread = false;
    strcpy(app_widgets->current_mode, "interval"); // Default mode
    gdk_rgba_parse(&app_widgets->status_circle_color, "red"); // Default color (offline)

    app_widgets->main_window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(app_widgets->main_window, "Load Cell Server (C/GTK+)");
    gtk_window_set_default_size(app_widgets->main_window, 650, 550);
    gtk_window_set_resizable(app_widgets->main_window, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(app_widgets->main_window), 20); // Padding

    g_signal_connect(app_widgets->main_window, "destroy", G_CALLBACK(on_main_window_destroy), NULL);


    // Apply styles before creating widgets for consistent appearance
    apply_styles();

    // Main Box for overall layout
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(app_widgets->main_window), main_vbox);

    // Title Label
    GtkWidget *title_label = gtk_label_new("Load Cell Data Server");
    gtk_widget_set_name(title_label, "title_label"); // Set CSS name for styling
    gtk_box_pack_start(GTK_BOX(main_vbox), title_label, FALSE, FALSE, 0);
    gtk_widget_set_margin_bottom(title_label, 20);

    // Server Status Indicator
    GtkWidget *status_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), status_hbox, FALSE, FALSE, 0);
    gtk_widget_set_margin_bottom(status_hbox, 15);

    app_widgets->status_indicator = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_size_request(GTK_WIDGET(app_widgets->status_indicator), 15, 15);
    g_signal_connect(G_OBJECT(app_widgets->status_indicator), "draw", G_CALLBACK(draw_status_indicator), NULL);
    gtk_box_pack_start(GTK_BOX(status_hbox), GTK_WIDGET(app_widgets->status_indicator), FALSE, FALSE, 0);
    gtk_widget_set_margin_end(GTK_WIDGET(app_widgets->status_indicator), 5);

    app_widgets->status_label = GTK_LABEL(gtk_label_new("Server Offline"));
    gtk_widget_set_name(GTK_WIDGET(app_widgets->status_label), "status_label_small");
    gtk_box_pack_start(GTK_BOX(status_hbox), GTK_WIDGET(app_widgets->status_label), FALSE, FALSE, 0);


    // Mode Selection Section
    GtkWidget *mode_frame = gtk_frame_new(" Select Sending Mode ");
    gtk_widget_set_name(mode_frame, "mode_frame"); // For CSS
    gtk_box_pack_start(GTK_BOX(main_vbox), mode_frame, FALSE, FALSE, 0);
    gtk_widget_set_margin_bottom(mode_frame, 15);

    GtkWidget *mode_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(mode_frame), mode_vbox);
    gtk_container_set_border_width(GTK_CONTAINER(mode_vbox), 10); // Padding inside frame

    app_widgets->radio_interval = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label(NULL, "Send files at fixed intervals"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app_widgets->radio_interval), TRUE); // Set as default
    g_signal_connect(G_OBJECT(app_widgets->radio_interval), "toggled", G_CALLBACK(update_mode_controls), NULL);
    gtk_box_pack_start(GTK_BOX(mode_vbox), GTK_WIDGET(app_widgets->radio_interval), FALSE, FALSE, 5);
    gtk_widget_set_margin_start(GTK_WIDGET(app_widgets->radio_interval), 5);

    app_widgets->radio_freq = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(
                                                GTK_RADIO_BUTTON(app_widgets->radio_interval),
                                                "Send specific file by frequency (Hz)"));
    g_signal_connect(G_OBJECT(app_widgets->radio_freq), "toggled", G_CALLBACK(update_mode_controls), NULL);
    gtk_box_pack_start(GTK_BOX(mode_vbox), GTK_WIDGET(app_widgets->radio_freq), FALSE, FALSE, 5);
    gtk_widget_set_margin_start(GTK_WIDGET(app_widgets->radio_freq), 5);

    app_widgets->radio_select_file = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(
                                                GTK_RADIO_BUTTON(app_widgets->radio_interval),
                                                "Select a file and send"));
    g_signal_connect(G_OBJECT(app_widgets->radio_select_file), "toggled", G_CALLBACK(update_mode_controls), NULL);
    gtk_box_pack_start(GTK_BOX(mode_vbox), GTK_WIDGET(app_widgets->radio_select_file), FALSE, FALSE, 5);
    gtk_widget_set_margin_start(GTK_WIDGET(app_widgets->radio_select_file), 5);

    // Input Controls Section
    GtkWidget *input_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(input_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(input_grid), 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), input_grid, FALSE, FALSE, 0);
    gtk_widget_set_margin_bottom(input_grid, 15);
    gtk_widget_set_margin_start(input_grid, 10);
    gtk_widget_set_margin_end(input_grid, 10);

    // Interval Selection
    app_widgets->label_interval_widget = GTK_LABEL(gtk_label_new("Sending Interval (ms):"));
    gtk_label_set_xalign(GTK_LABEL(app_widgets->label_interval_widget), 0.0); // Align left
    gtk_grid_attach(GTK_GRID(input_grid), GTK_WIDGET(app_widgets->label_interval_widget), 0, 0, 1, 1);

    app_widgets->interval_menu = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "1");
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "2");
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "5");
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "10");
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "20");
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "50");
    gtk_combo_box_text_append_text(app_widgets->interval_menu, "100");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app_widgets->interval_menu), "20");
    gtk_grid_attach(GTK_GRID(input_grid), GTK_WIDGET(app_widgets->interval_menu), 1, 0, 1, 1);
    gtk_widget_set_hexpand(GTK_WIDGET(app_widgets->interval_menu), TRUE); // Expand horizontally

    // Frequency Selection
    app_widgets->label_freq_widget = GTK_LABEL(gtk_label_new("Frequency (Hz):"));
    gtk_label_set_xalign(GTK_LABEL(app_widgets->label_freq_widget), 0.0);
    gtk_grid_attach(GTK_GRID(input_grid), GTK_WIDGET(app_widgets->label_freq_widget), 0, 1, 1, 1);

    app_widgets->freq_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(app_widgets->freq_entry, "50");
    gtk_grid_attach(GTK_GRID(input_grid), GTK_WIDGET(app_widgets->freq_entry), 1, 1, 1, 1);
    gtk_widget_set_hexpand(GTK_WIDGET(app_widgets->freq_entry), TRUE);

    // File Selection Button
    app_widgets->select_file_button = GTK_BUTTON(gtk_button_new_with_label("Select File"));
    gtk_widget_set_name(GTK_WIDGET(app_widgets->select_file_button), "accent_button");
    g_signal_connect(G_OBJECT(app_widgets->select_file_button), "clicked", G_CALLBACK(select_file_clicked), NULL);
    gtk_grid_attach(GTK_GRID(input_grid), GTK_WIDGET(app_widgets->select_file_button), 0, 2, 2, 1);
    gtk_widget_set_margin_top(GTK_WIDGET(app_widgets->select_file_button), 10);
    
    app_widgets->selected_file_label = GTK_LABEL(gtk_label_new("No file selected"));
    gtk_widget_set_name(GTK_WIDGET(app_widgets->selected_file_label), "selected_file_display_label");
    gtk_label_set_xalign(GTK_LABEL(app_widgets->selected_file_label), 0.0);
    gtk_grid_attach(GTK_GRID(input_grid), GTK_WIDGET(app_widgets->selected_file_label), 0, 3, 2, 1);
    gtk_widget_set_margin_bottom(GTK_WIDGET(app_widgets->selected_file_label), 5);


    // Start Button
    app_widgets->start_button = GTK_BUTTON(gtk_button_new_with_label("Start Sending Data"));
    gtk_widget_set_name(GTK_WIDGET(app_widgets->start_button), "primary_button");
    g_signal_connect(G_OBJECT(app_widgets->start_button), "clicked", G_CALLBACK(start_sending_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(main_vbox), GTK_WIDGET(app_widgets->start_button), FALSE, FALSE, 0);
    gtk_widget_set_margin_top(GTK_WIDGET(app_widgets->start_button), 20);
    gtk_widget_set_margin_start(GTK_WIDGET(app_widgets->start_button), 10);
    gtk_widget_set_margin_end(GTK_WIDGET(app_widgets->start_button), 10);

    app_widgets->overall_status_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_name(GTK_WIDGET(app_widgets->overall_status_label), "overall_status_label_big");
    gtk_box_pack_start(GTK_BOX(main_vbox), GTK_WIDGET(app_widgets->overall_status_label), FALSE, FALSE, 0);
    gtk_widget_set_margin_top(GTK_WIDGET(app_widgets->overall_status_label), 10);

    // Initial update of controls based on default mode
    update_mode_controls();

    gtk_widget_show_all(GTK_WIDGET(app_widgets->main_window));
}

int main(int argc, char **argv) {
    // Create the 'June23' folder if it doesn't exist
    #if defined(_WIN32) || defined(_WIN64)
        _mkdir(FOLDER); // For Windows, ensures folder exists for files
    #else
        mkdir(FOLDER, 0777); // For Unix-like systems (Linux, macOS)
    #endif

    // Initialize Winsock for Windows (one-time per process)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed in main: %d\n", WSAGetLastError());
        return 1;
    }

    GtkApplication *app;
    int status;

    app = gtk_application_new("com.example.loadcellserver", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app); // Release the application instance

    // Cleanup Winsock after the application exits
    WSACleanup();

    return status;
}