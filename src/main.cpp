#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_log.h"

// Application includes
#include "config/logging.h"
#include "hardware/hardware_manager.h"
#include "system/state_machine.h"
#include "system/statistics_manager.h"
#include "controllers/profile_controller.h"
#include "controllers/grind_controller.h"
#include "ui/ui_manager.h"
#include "config/constants.h"
#include "bluetooth/manager.h"
#include "tasks/task_manager.h"
#include "tasks/weight_sampling_task.h"
#include "tasks/grind_control_task.h"
#include "tasks/file_io_task.h"

HardwareManager hardware_manager;
StateMachine state_machine;
ProfileController profile_controller;
GrindController grind_controller;
UIManager ui_manager;
BluetoothManager g_bluetooth_manager;
BluetoothManager& bluetooth_manager = g_bluetooth_manager;

#if SYS_ENABLE_REALTIME_HEARTBEAT
// Core 1 timing metrics (global scope for main loop access)
static uint32_t core1_cycle_count_10s = 0;
static uint32_t core1_cycle_time_sum_ms = 0;
static uint32_t core1_cycle_time_min_ms = UINT32_MAX;
static uint32_t core1_cycle_time_max_ms = 0;
static uint32_t core1_last_heartbeat_time = 0;
#endif

void app_main(void) {
    // Initialize logging system
    logging_init();
    
    LOG_BLE("[STARTUP] Initializing ESP32-S3 Coffee Scale - Build %d\n", BUILD_NUMBER);
    
    hardware_manager.init();
    profile_controller.init(hardware_manager.get_preferences());
    statistics_manager.init(hardware_manager.get_preferences());
    grind_controller.init(hardware_manager.get_load_cell(), hardware_manager.get_grinder(), hardware_manager.get_preferences());
    
    // Set up the reference so HardwareManager can query GrindController state
    hardware_manager.set_grind_controller(&grind_controller);
    
    bluetooth_manager.init(hardware_manager.get_preferences());
    
    // Check for OTA failure to determine initial state
    String failed_ota_build = bluetooth_manager.check_ota_failure_after_boot();
    bool ota_failed = !failed_ota_build.isEmpty();

    // Check calibration status to determine initial screen
    bool is_calibrated = hardware_manager.get_weight_sensor()->is_calibrated();

    if (ota_failed) {
        LOG_BLE("BOOT: Starting in OTA failure state for expected build %s\n", failed_ota_build.c_str());
        state_machine.init(UIState::OTA_UPDATE_FAILED);
    } else if (!is_calibrated) {
        LOG_BLE("BOOT: Device not calibrated - starting in CALIBRATION state\n");
        state_machine.init(UIState::CALIBRATION);
    } else {
        state_machine.init(UIState::READY);
    }
    
    ui_manager.init(&hardware_manager, &state_machine, &profile_controller, &grind_controller, &bluetooth_manager);
    
    // Store OTA failure info in ui_manager if needed
    if (ota_failed) {
        if (auto* ota = ui_manager.get_ota_data_export_controller()) {
            ota->set_failure_info(failed_ota_build.c_str());
        }
    }
    
    // Set up UI status callback to avoid circular dependency
    bluetooth_manager.set_ui_status_callback([](const char* status) {
        if (auto* ota = ui_manager.get_ota_data_export_controller()) {
            ota->update_status(status);
        }
    });
    
    // Enable BLE by default during bootup with 2-minute timeout
    // (Previously disabled by default for security, now enabled for user convenience)
    bluetooth_manager.enable_during_bootup();
    
    // Initialize individual task modules BEFORE TaskManager creates FreeRTOS tasks
    // This ensures all task dependencies are ready before tasks start running
    LOG_BLE("[STARTUP] Initializing task module dependencies...\n");
    weight_sampling_task.init(hardware_manager.get_load_cell(), &grind_logger);
    grind_control_task.init(&grind_controller, hardware_manager.get_load_cell(), 
                           hardware_manager.get_grinder(), &grind_logger);
    
    LOG_BLE("✅ Task module dependencies initialized\n");
    
    // Initialize TaskManager with hardware and system interfaces
    LOG_BLE("[STARTUP] Initializing FreeRTOS Task Architecture...\n");
    bool task_init_success = task_manager.init(&hardware_manager, &state_machine, &profile_controller, 
                                              &grind_controller, &bluetooth_manager, &ui_manager);
    
    if (!task_init_success) {
        LOG_BLE("ERROR: Failed to initialize TaskManager - system cannot start\n");
        while (true) {
            delay(1000); // Halt system if task initialization fails
        }
    }
    
    LOG_BLE("✅ TaskManager initialized successfully\n");
    
    // Initialize remaining task modules that depend on TaskManager queues
    file_io_task.init(task_manager.get_file_io_queue());
    
    LOG_BLE("✅ All task modules initialized\n");
    
    // Main application loop - runs on core that calls app_main
    // All heavy lifting is now done by FreeRTOS tasks
    #if SYS_ENABLE_REALTIME_HEARTBEAT
    uint32_t core1_cycle_count_10s = 0;
    uint32_t core1_cycle_time_sum_ms = 0;
    uint32_t core1_cycle_time_min_ms = UINT32_MAX;
    uint32_t core1_cycle_time_max_ms = 0;
    uint32_t core1_last_heartbeat_time = 0;
    #endif
    
    uint32_t last_uptime_update = 0;
    uint32_t pending_uptime_minutes = 0;
    bool hardware_suspended = false;
    
    while (true) {
        #if SYS_ENABLE_REALTIME_HEARTBEAT
        uint32_t cycle_start_time = esp_timer_get_time() / 1000;  // Convert to ms
        core1_cycle_count_10s++;
        if (core1_last_heartbeat_time == 0) core1_last_heartbeat_time = cycle_start_time;
        #endif
        
        // Update device uptime statistics every 15 minutes
        uint32_t current_time = esp_timer_get_time() / 1000;  // Convert to ms
        if (last_uptime_update == 0) {
            last_uptime_update = current_time;
        }
        constexpr uint32_t kUptimeIntervalMs = 900000; // 15 minutes
        constexpr uint32_t kUptimeIntervalMinutes = 15;
        uint32_t elapsed_ms = current_time - last_uptime_update;
        if (elapsed_ms >= kUptimeIntervalMs) {
            uint32_t intervals = elapsed_ms / kUptimeIntervalMs;
            pending_uptime_minutes += intervals * kUptimeIntervalMinutes;
            last_uptime_update += intervals * kUptimeIntervalMs;

            if (pending_uptime_minutes > 0) {
                statistics_manager.update_uptime(pending_uptime_minutes);
                pending_uptime_minutes = 0;
            }
        }
        
        // Check OTA state and suspend hardware tasks if needed
        bool ota_active = bluetooth_manager.is_updating();
        
        if (ota_active && !hardware_suspended) {
            task_manager.suspend_hardware_tasks();
            hardware_suspended = true;
            LOG_BLE("[MAIN] Hardware tasks suspended for OTA\n");
        } else if (!ota_active && hardware_suspended) {
            task_manager.resume_hardware_tasks();
            hardware_suspended = false;
            LOG_BLE("[MAIN] Hardware tasks resumed after OTA\n");
        }
        
        #if SYS_ENABLE_REALTIME_HEARTBEAT
        // Calculate Core 1 main loop timing
        uint32_t cycle_end_time = esp_timer_get_time() / 1000;  // Convert to ms
        uint32_t cycle_duration = cycle_end_time - cycle_start_time;
        core1_cycle_time_sum_ms += cycle_duration;
        if (cycle_duration < core1_cycle_time_min_ms) core1_cycle_time_min_ms = cycle_duration;
        if (cycle_duration > core1_cycle_time_max_ms) core1_cycle_time_max_ms = cycle_duration;
        
        // Core 1 Main Loop Heartbeat - Monitor main loop health (every 10 seconds)
        if (cycle_end_time - core1_last_heartbeat_time >= SYS_REALTIME_HEARTBEAT_INTERVAL_MS) {
            uint32_t avg_cycle_time = core1_cycle_count_10s > 0 ? core1_cycle_time_sum_ms / core1_cycle_count_10s : 0;
            
            // Get system states
            bool is_grinding = grind_controller.is_active();
            const char* ble_state = bluetooth_manager.is_enabled() ? 
                                   (bluetooth_manager.is_connected() ? "CONN" : "ADV") : "OFF";
            const char* grinder_state = is_grinding ? "ACTIVE" : "IDLE";
            const char* tasks_status = task_manager.are_tasks_healthy() ? "HEALTHY" : "ERROR";
            size_t free_heap_kb = esp_get_free_heap_size() / 1024;
            
            LOG_BLE("[%lums MAIN_LOOP_HEARTBEAT] Cycles: %lu/10s | Avg: %lums (%lu-%lums) | Tasks: %s | BLE: %s | Grinder: %s | Mem: %zuKB | Build: #%d\n",
                   current_time, core1_cycle_count_10s, avg_cycle_time, core1_cycle_time_min_ms, core1_cycle_time_max_ms,
                   tasks_status, ble_state, grinder_state, free_heap_kb, BUILD_NUMBER);
            
            // Reset Core 1 metrics for next interval
            core1_cycle_count_10s = 0;
            core1_cycle_time_sum_ms = 0;
            core1_cycle_time_min_ms = UINT32_MAX;
            core1_cycle_time_max_ms = 0;
            core1_last_heartbeat_time = cycle_end_time;
        }
        #endif
        
        // Yield to allow FreeRTOS scheduler to run other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
