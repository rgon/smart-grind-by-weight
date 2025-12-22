// Migrated from Arduino LittleFS to ESP-IDF VFS + POSIX
#include "data_stream.h"
#include "../logging/grind_logging.h"
#include "../config/constants.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>

extern GrindLogger grind_logger;

DataStreamManager::DataStreamManager() 
    : current_session_id(0)
    , file_bytes_sent(0)
    , file_total_size(0)
    , file_stream_active(false) {
}

DataStreamManager::~DataStreamManager() {
    close_stream();
}

uint32_t DataStreamManager::get_total_sessions() const {
    return grind_logger.get_total_flash_sessions();
}

void DataStreamManager::close_stream() {
    if (file_stream_active) {
        LOG_BLE("DataStream: Closing file stream\n");
    }
    if (active_file) {
        fclose(active_file);
        active_file = nullptr;
    }
    file_stream_active = false;
    current_session_id = 0;
    file_bytes_sent = 0;
    file_total_size = 0;
}

uint32_t DataStreamManager::get_session_list(uint32_t* session_ids, uint32_t max_sessions) {
    uint32_t total_sessions = grind_logger.count_sessions_in_flash();
    if (total_sessions == 0 || !session_ids) {
        return 0;
    }
    
    // Reuse the session list logic from export_sessions_binary_chunk
    uint32_t* session_list = (uint32_t*)heap_caps_malloc(total_sessions * sizeof(uint32_t), MALLOC_CAP_8BIT);
    if (!session_list) {
        LOG_BLE("ERROR: Failed to allocate session list memory\n");
        return 0;
    }
    
    uint32_t list_count = 0;
    
    // Try individual session files first (new approach)
    if (access(GRIND_SESSIONS_DIR, F_OK) == 0) {
        DIR* dir = opendir(GRIND_SESSIONS_DIR);
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr && list_count < total_sessions) {
                const char* fname = entry->d_name;
                size_t len = strlen(fname);
                bool is_session_file = (len > 12 && strncmp(fname, "session_", 8) == 0 && strcmp(fname + len - 4, ".bin") == 0);
                if (is_session_file) {
                    // Extract numeric ID between '_' and '.'
                    const char* underscore = strchr(fname, '_');
                    const char* dot = strrchr(fname, '.');
                    if (underscore && dot && underscore < dot) {
                        char idbuf[16] = {0};
                        size_t idlen = (size_t)(dot - underscore - 1);
                        if (idlen < sizeof(idbuf)) {
                            memcpy(idbuf, underscore + 1, idlen);
                            uint32_t sid = (uint32_t)strtoul(idbuf, nullptr, 10);
                            if (sid > 0) {
                                session_list[list_count++] = sid;
                            }
                        }
                    }
                }
            }
            closedir(dir);
        }
    }
    
    // Sort session IDs for consistent order
    for (uint32_t i = 0; i < list_count - 1; i++) {
        for (uint32_t j = i + 1; j < list_count; j++) {
            if (session_list[i] > session_list[j]) {
                uint32_t temp = session_list[i];
                session_list[i] = session_list[j];
                session_list[j] = temp;
            }
        }
    }
    
    // Copy results to output array
    uint32_t copy_count = (list_count < max_sessions) ? list_count : max_sessions;
    for (uint32_t i = 0; i < copy_count; i++) {
        session_ids[i] = session_list[i];
    }
    
    heap_caps_free(session_list);
    LOG_BLE("DataStream: Found %lu session files\n", list_count);
    return copy_count;
}

bool DataStreamManager::initialize_file_stream(uint32_t session_id) {
    // Close any existing file stream
    close_stream();

    current_session_id = session_id;
    file_bytes_sent = 0;

    // Get file size to estimate total transfer
    char filename[64];
    snprintf(filename, sizeof(filename), SESSION_FILE_FORMAT, session_id);

    if (access(filename, F_OK) != 0) {
        LOG_BLE("ERROR: Session file %s does not exist\n", filename);
        return false;
    }

    active_file = fopen(filename, "rb");
    if (!active_file) {
        LOG_BLE("ERROR: Failed to open session file %s\n", filename);
        return false;
    }

    // Determine file size
    if (fseek(active_file, 0L, SEEK_END) != 0) {
        fclose(active_file);
        active_file = nullptr;
        return false;
    }
    long sz = ftell(active_file);
    if (sz < 0) sz = 0;
    file_total_size = (uint32_t)sz;
    fseek(active_file, 0L, SEEK_SET);
    LOG_BLE("DataStream: Initialized file stream for session %lu (%lu bytes)\n", session_id, file_total_size);
    file_stream_active = true;
    return true;
}

bool DataStreamManager::read_file_chunk(uint8_t* buffer, size_t buffer_size, size_t* actual_size) {
    if (!file_stream_active || !buffer || !actual_size) {
        return false;
    }

    if (!active_file) {
        LOG_BLE("ERROR: Active file handle missing for session %lu\n", current_session_id);
        file_stream_active = false;
        return false;
    }

    // Read next chunk at current file position
    size_t bytes_read = fread(buffer, 1, buffer_size, active_file);

    if (bytes_read > 0) {
        file_bytes_sent += bytes_read;
        *actual_size = bytes_read;

        // Check if file is complete
        if (file_bytes_sent >= file_total_size || feof(active_file)) {
            LOG_BLE("DataStream: Completed file stream for session %lu\n", current_session_id);
            fclose(active_file);
            active_file = nullptr;
            file_stream_active = false;
        }

        return true;
    }

    // No more data
    LOG_BLE("DataStream: End of file stream for session %lu\n", current_session_id);
    fclose(active_file);
    active_file = nullptr;
    file_stream_active = false;
    return false;
}

uint8_t DataStreamManager::get_progress_percent() const {
    if (!file_stream_active || file_total_size == 0) {
        return 0;
    }
    
    // Calculate progress percentage (0-100)
    uint32_t progress = (file_bytes_sent * 100) / file_total_size;
    
    // Ensure we don't exceed 100%
    return (progress > 100) ? 100 : static_cast<uint8_t>(progress);
}
