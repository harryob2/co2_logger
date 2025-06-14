#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/view.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <datetime/datetime.h>
#include "co2_logger.h"

#define CSV_FILE_PATH "/ext/apps_data/co2_logger/co2_log.csv"
#define LOG_INTERVAL_MS (30 * 1000) // 30 seconds

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    NotificationApp* notification;
    Storage* storage;
    File* csv_file;
    co2_logger* co2_logger;
    FuriMutex* mutex;
    uint32_t last_log_time;
    uint32_t co2_value;
    bool connected;
    bool csv_enabled;
} co2_loggerUart;

static bool co2_logger_uart_check_sd_card(co2_loggerUart* state) {
    FURI_LOG_D("CSV", "Checking SD card status...");
    
    // Check if SD card is mounted
    FS_Error sd_status = storage_sd_status(state->storage);
    if(sd_status != FSE_OK) {
        FURI_LOG_W("CSV", "SD card not ready, status: %s", storage_error_get_desc(sd_status));
        return false;
    }
    
    FURI_LOG_D("CSV", "SD card is ready");
    return true;
}

static bool co2_logger_uart_create_csv_file(co2_loggerUart* state) {
    FURI_LOG_I("CSV", "Setting up CSV logging...");
    
    // Check SD card first
    if(!co2_logger_uart_check_sd_card(state)) {
        FURI_LOG_W("CSV", "SD card not available - disabling CSV logging");
        return false;
    }
    
    // Create directories step by step with proper error checking
    FURI_LOG_D("CSV", "Creating directory structure...");
    
    FS_Error dir_result = storage_common_mkdir(state->storage, "/ext/apps_data");
    if(dir_result != FSE_OK && dir_result != FSE_EXIST) {
        FURI_LOG_E("CSV", "Failed to create /ext/apps_data: %s", storage_error_get_desc(dir_result));
        return false;
    }
    
    dir_result = storage_common_mkdir(state->storage, "/ext/apps_data/co2_logger");
    if(dir_result != FSE_OK && dir_result != FSE_EXIST) {
        FURI_LOG_E("CSV", "Failed to create /ext/apps_data/co2_logger: %s", storage_error_get_desc(dir_result));
        return false;
    }
    
    FURI_LOG_D("CSV", "Directory structure created successfully");
    
    // Allocate file handle
    state->csv_file = storage_file_alloc(state->storage);
    if(!state->csv_file) {
        FURI_LOG_E("CSV", "Failed to allocate file handle");
        return false;
    }
    
    // Check if file exists
    bool file_exists = storage_file_exists(state->storage, CSV_FILE_PATH);
    FURI_LOG_D("CSV", "File exists: %s", file_exists ? "YES" : "NO");
    
    if(file_exists) {
        // File exists, open for append
        FURI_LOG_D("CSV", "Opening existing file for append...");
        if(!storage_file_open(state->csv_file, CSV_FILE_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
            FURI_LOG_E("CSV", "Failed to open file for append: %s", 
                       storage_file_get_error_desc(state->csv_file));
            storage_file_free(state->csv_file);
            state->csv_file = NULL;
            return false;
        }
        FURI_LOG_I("CSV", "CSV file opened for append");
    } else {
        // File doesn't exist, create new one
        FURI_LOG_D("CSV", "Creating new CSV file...");
        if(!storage_file_open(state->csv_file, CSV_FILE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            FURI_LOG_E("CSV", "Failed to create new file: %s", 
                       storage_file_get_error_desc(state->csv_file));
            storage_file_free(state->csv_file);
            state->csv_file = NULL;
            return false;
        }
        
        // Write header
        const char* header = "Timestamp,CO2_PPM,Status\n";
        size_t header_len = strlen(header);
        size_t written = storage_file_write(state->csv_file, header, header_len);
        
        if(written != header_len) {
            FURI_LOG_E("CSV", "Failed to write header (wrote %zu of %zu bytes)", written, header_len);
            storage_file_close(state->csv_file);
            storage_file_free(state->csv_file);
            state->csv_file = NULL;
            return false;
        }
        
        // Force write to disk
        if(!storage_file_sync(state->csv_file)) {
            FURI_LOG_W("CSV", "Warning: Failed to sync file after header write");
        }
        
        FURI_LOG_I("CSV", "New CSV file created with header");
    }
    
    return true;
}

static void co2_logger_uart_log_to_csv(co2_loggerUart* state, uint32_t co2_value, bool connected) {
    if(!state->csv_enabled || !state->csv_file) {
        FURI_LOG_D("CSV", "CSV logging disabled or file not open");
        return;
    }
    
    FURI_LOG_D("CSV", "Logging to CSV: %lu ppm, connected: %s", co2_value, connected ? "true" : "false");
    
    // Get current timestamp using the correct DateTime API
    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);
    
    // Format: YYYY-MM-DD HH:MM:SS,CO2_PPM,Status
    char log_entry[128];
    int entry_len = snprintf(log_entry, sizeof(log_entry), 
                            "%04d-%02d-%02d %02d:%02d:%02d,%lu,%s\n",
                            datetime.year, datetime.month, datetime.day,
                            datetime.hour, datetime.minute, datetime.second,
                            co2_value, connected ? "Connected" : "Disconnected");
    
    if(entry_len < 0 || entry_len >= (int)sizeof(log_entry)) {
        FURI_LOG_E("CSV", "Failed to format log entry");
        return;
    }
    
    FURI_LOG_D("CSV", "Writing entry: %s", log_entry);
    
    size_t written = storage_file_write(state->csv_file, log_entry, entry_len);
    if(written != (size_t)entry_len) {
        FURI_LOG_E("CSV", "Failed to write log entry (wrote %zu of %d bytes)", written, entry_len);
        
        // Get detailed error info
        FS_Error error = storage_file_get_error(state->csv_file);
        FURI_LOG_E("CSV", "File error: %s", storage_error_get_desc(error));
        
        // If write failed, disable CSV to prevent further errors
        FURI_LOG_W("CSV", "Disabling CSV logging due to write failure");
        state->csv_enabled = false;
        return;
    }
    
    // Sync every write to ensure data is saved
    if(!storage_file_sync(state->csv_file)) {
        FURI_LOG_W("CSV", "Warning: Failed to sync file after write");
    }
    
    FURI_LOG_D("CSV", "Successfully logged entry to CSV");
}

static void co2_logger_uart_draw_callback(Canvas* canvas, void* ctx) {
    co2_loggerUart* app = (co2_loggerUart*)ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "CO2 Logger");

    canvas_set_font(canvas, FontSecondary);
    if(app->connected) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "CO2: %lu ppm", app->co2_value);
        canvas_draw_str(canvas, 2, 25, buffer);
        canvas_draw_str(canvas, 2, 35, "Status: Connected");
    } else {
        canvas_draw_str(canvas, 2, 25, "CO2: --- ppm");
        canvas_draw_str(canvas, 2, 35, "Status: Disconnected");
    }
    
    canvas_draw_str(canvas, 2, 45, "Logging every 30s");
    
    // Show CSV status
    if(app->csv_enabled) {
        canvas_draw_str(canvas, 2, 55, "CSV: Enabled");
    } else {
        canvas_draw_str(canvas, 2, 55, "CSV: Disabled");
    }
    
    canvas_draw_str(canvas, 2, 65, "Press BACK to exit");

    furi_mutex_release(app->mutex);
}

static void co2_logger_uart_input_callback(InputEvent* input_event, void* ctx) {
    co2_loggerUart* app = (co2_loggerUart*)ctx;
    furi_message_queue_put(app->event_queue, input_event, FuriWaitForever);
}

int32_t co2_logger_app(void* p) {
    UNUSED(p);
    
    // Log immediately at start
    FURI_LOG_E("App", "=== CO2 LOGGER APP STARTING ===");
    
    // Allocate state on heap instead of stack to prevent stack overflow
    co2_loggerUart* state = malloc(sizeof(co2_loggerUart));
    if(!state) {
        FURI_LOG_E("App", "Failed to allocate main state structure");
        return -1;
    }
    memset(state, 0, sizeof(co2_loggerUart));
    
    FURI_LOG_I("App", "Opening GUI record...");
    state->gui = furi_record_open(RECORD_GUI);
    if(!state->gui) {
        FURI_LOG_E("App", "Failed to open GUI record");
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Opening notification record...");
    state->notification = furi_record_open(RECORD_NOTIFICATION);
    if(!state->notification) {
        FURI_LOG_E("App", "Failed to open notification record");
        furi_record_close(RECORD_GUI);
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Opening storage record...");
    state->storage = furi_record_open(RECORD_STORAGE);
    if(!state->storage) {
        FURI_LOG_E("App", "Failed to open storage record");
        furi_record_close(RECORD_NOTIFICATION);
        furi_record_close(RECORD_GUI);
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Allocating view port...");
    state->view_port = view_port_alloc();
    if(!state->view_port) {
        FURI_LOG_E("App", "Failed to allocate view port");
        furi_record_close(RECORD_STORAGE);
        furi_record_close(RECORD_NOTIFICATION);
        furi_record_close(RECORD_GUI);
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Allocating event queue...");
    state->event_queue = furi_message_queue_alloc(32, sizeof(InputEvent));
    if(!state->event_queue) {
        FURI_LOG_E("App", "Failed to allocate event queue");
        view_port_free(state->view_port);
        furi_record_close(RECORD_STORAGE);
        furi_record_close(RECORD_NOTIFICATION);
        furi_record_close(RECORD_GUI);
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Allocating mutex...");
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!state->mutex) {
        FURI_LOG_E("App", "Failed to allocate mutex");
        furi_message_queue_free(state->event_queue);
        view_port_free(state->view_port);
        furi_record_close(RECORD_STORAGE);
        furi_record_close(RECORD_NOTIFICATION);
        furi_record_close(RECORD_GUI);
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Allocating CO2 logger...");
    state->co2_logger = co2_logger_alloc();
    if(!state->co2_logger) {
        FURI_LOG_E("App", "Failed to allocate CO2 logger");
        furi_mutex_free(state->mutex);
        furi_message_queue_free(state->event_queue);
        view_port_free(state->view_port);
        furi_record_close(RECORD_STORAGE);
        furi_record_close(RECORD_NOTIFICATION);
        furi_record_close(RECORD_GUI);
        free(state);
        return -1;
    }
    
    FURI_LOG_I("App", "Setting up CSV logging...");
    
    // Try to set up CSV logging with robust error handling
    if(co2_logger_uart_create_csv_file(state)) {
        state->csv_enabled = true;
        FURI_LOG_I("App", "CSV logging enabled successfully");
    } else {
        state->csv_enabled = false;
        state->csv_file = NULL;
        FURI_LOG_W("App", "CSV logging disabled - continuing without CSV");
    }

    FURI_LOG_I("App", "Setting up view port callbacks...");
    view_port_draw_callback_set(state->view_port, co2_logger_uart_draw_callback, state);
    view_port_input_callback_set(state->view_port, co2_logger_uart_input_callback, state);

    FURI_LOG_I("App", "Adding view port to GUI...");
    gui_add_view_port(state->gui, state->view_port, GuiLayerFullscreen);

    FURI_LOG_I("App", "Opening CO2 logger connection...");
    co2_logger_open(state->co2_logger);

    FURI_LOG_I("App", "=== APP SETUP COMPLETE - ENTERING MAIN LOOP ===");

    InputEvent event;
    bool running = true;
    uint32_t last_measurement_time = 0;

    while(running) {
        FuriStatus status = furi_message_queue_get(state->event_queue, &event, 100);
        
        uint32_t current_time = furi_get_tick();
        
        // Check for input events
        if(status == FuriStatusOk) {
            FURI_LOG_D("Input", "Received input: type=%d key=%d", event.type, event.key);
            if(event.type == InputTypePress && event.key == InputKeyBack) {
                FURI_LOG_I("App", "Back button pressed, exiting");
                running = false;
            }
        }
        
        // Take CO2 measurement every second for display
        if(current_time - last_measurement_time > 1000) {
            FURI_LOG_D("Measure", "Taking CO2 measurement...");
            furi_mutex_acquire(state->mutex, FuriWaitForever);
            
            state->connected = co2_logger_read_gas_concentration(state->co2_logger, &state->co2_value);
            if(state->connected) {
                FURI_LOG_I("Measure", "CO2 reading: %lu ppm", state->co2_value);
            } else {
                FURI_LOG_W("Measure", "Failed to read CO2 sensor");
            }
            
            furi_mutex_release(state->mutex);
            last_measurement_time = current_time;
            
            // Log to CSV every 30 seconds
            if(current_time - state->last_log_time > LOG_INTERVAL_MS) {
                if(state->csv_enabled) {
                    co2_logger_uart_log_to_csv(state, state->co2_value, state->connected);
                } else {
                    FURI_LOG_D("CSV", "CSV logging disabled - skipping log entry");
                }
                state->last_log_time = current_time;
            }
            
            view_port_update(state->view_port);
        }
    }

    FURI_LOG_I("App", "=== CLEANING UP AND EXITING ===");

    // Cleanup
    gui_remove_view_port(state->gui, state->view_port);
    co2_logger_close(state->co2_logger);
    co2_logger_free(state->co2_logger);
    
    if(state->csv_file) {
        FURI_LOG_I("CSV", "Closing CSV file");
        storage_file_close(state->csv_file);
        storage_file_free(state->csv_file);
    }
    
    furi_mutex_free(state->mutex);
    furi_message_queue_free(state->event_queue);
    view_port_free(state->view_port);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(state);

    FURI_LOG_I("App", "=== CO2 LOGGER APP EXITED SUCCESSFULLY ===");
    return 0;
}
