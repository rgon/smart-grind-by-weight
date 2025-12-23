// BLE Manager stub implementation
// Full implementation disabled due to NimBLE header resolution issues
// This file provides minimal linked symbols to allow the project to build

#include "manager.h"
#include "../system/Preferences.h"
#include <cstdarg>
#include <esp_log.h>

static const char* TAG = "BLEManager";

BluetoothManager::BluetoothManager() 
        : device_connected(false),
            ble_enabled(false),
            debug_stream_active(false),
            enable_time(0),
            timeout_ms(60000),
            last_disconnect_time(0),
            data_export_in_progress(false),
            data_status(BLE_DATA_IDLE),
            current_chunk(0),
            next_chunk_time(0),
            current_file_session_id(0),
            sessions_info_dirty(false),
            last_session_storage_version(0),
            last_reported_export_state(false),
            ui_status_queue(nullptr),
            diagnostic_report_pending(false),
            diagnostic_report_in_progress(false) {
}

BluetoothManager::~BluetoothManager() {
}

void BluetoothManager::init(Preferences* prefs) {
    ESP_LOGI(TAG, "BLE initialization skipped (NimBLE headers unresolved)");
    // TODO: Re-enable when NimBLE headers are fixed
}

void BluetoothManager::enable(unsigned long timeout_ms_arg) {
    ble_enabled = true;
    enable_time = esp_timer_get_time() / 1000;
    timeout_ms = timeout_ms_arg > 0 ? timeout_ms_arg : 60000;
    ESP_LOGI(TAG, "BLE enabled (stub - no actual BLE)");
}

void BluetoothManager::enable_during_bootup() {
    enable(30000);  // 30 second timeout during bootup
    ESP_LOGI(TAG, "BLE enabled during bootup (stub)");
}

void BluetoothManager::disable() {
    ble_enabled = false;
    device_connected = false;
    debug_stream_active = false;
    ESP_LOGI(TAG, "BLE disabled");
}

void BluetoothManager::handle() {
    // Stub - no actual BLE operations
}

void BluetoothManager::set_ui_status_callback(UIStatusCallback callback) {
    ui_status_callback = callback;
}

void BluetoothManager::update_ui_status(const char* status) {
    if (ui_status_callback) {
        ui_status_callback(status);
    }
}

void BluetoothManager::enqueue_ui_status(const char* status) {
    // Stub - no actual queuing
}

bool BluetoothManager::dequeue_ui_status(char* out, size_t out_len) {
    return false;
}

void BluetoothManager::start_advertising() {
    // Stub - no actual BLE advertising
}

void BluetoothManager::stop_advertising() {
    // Stub - no actual BLE advertising
}

void BluetoothManager::update_data_export() {
    // Stub - no data export without BLE
}

void BluetoothManager::send_next_data_chunk() {
    // Stub
}

void BluetoothManager::send_measurement_count() {
    // Stub
}

void BluetoothManager::send_log_message(const char* message) {
    ESP_LOGI(TAG, "Log: %s", message);
}

void BluetoothManager::clear_measurement_data() {
    // Stub
}

void BluetoothManager::send_file_list() {
    // Stub
}

void BluetoothManager::send_individual_file(uint32_t session_id) {
    // Stub
}

void BluetoothManager::set_ota_status(BLEOTAStatus status) {
    // Stub
}

void BluetoothManager::set_data_status(BLEDataStatus status) {
    data_status = status;
}

void BluetoothManager::update_system_info() {
    // Stub
}

void BluetoothManager::update_performance_info() {
    // Stub
}

void BluetoothManager::update_hardware_info() {
    // Stub
}

bool BluetoothManager::update_sessions_info() {
    return false;
}

void BluetoothManager::mark_sessions_info_dirty() {
    sessions_info_dirty = true;
}

void BluetoothManager::process_sessions_info_updates() {
    // Stub
}

void BluetoothManager::generate_diagnostic_report() {
    // Stub
}

void BluetoothManager::refresh_system_info() {
    update_system_info();
    update_performance_info();
    update_hardware_info();
    update_sessions_info();
}

void BluetoothManager::on_connect(uint16_t conn) {
    device_connected = true;
    ESP_LOGI(TAG, "Device connected (conn: %u)", conn);
}

void BluetoothManager::on_disconnect(uint16_t conn, int reason) {
    device_connected = false;
    last_disconnect_time = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Device disconnected (conn: %u, reason: %d)", conn, reason);
}

void BluetoothManager::handle_ota_data_chunk(const uint8_t* data, uint16_t len) {
    // Stub - OTA handled separately
}

void BluetoothManager::handle_ota_control_command(const uint8_t* data, uint16_t len) {
    // Stub - OTA handled separately
}

void BluetoothManager::handle_data_control_command(const uint8_t* data, uint16_t len) {
    // Stub - no data export without BLE
}

void BluetoothManager::handle_debug_command(const uint8_t* data, uint16_t len) {
    // Stub
}

float BluetoothManager::get_data_export_progress() const {
    return data_export_in_progress ? 100.0f : 0.0f;
}

unsigned long BluetoothManager::get_remaining_time_ms() const {
    return 0;
}

uint32_t BluetoothManager::get_data_export_session_count() const {
    return 0;
}

void BluetoothManager::start_data_export() {
    data_export_in_progress = true;
    set_data_status(BLE_DATA_EXPORTING);
}

void BluetoothManager::stop_data_export() {
    data_export_in_progress = false;
    set_data_status(BLE_DATA_IDLE);
}

unsigned long BluetoothManager::get_bluetooth_timeout_remaining_ms() const {
    if (!ble_enabled) return 0;
    unsigned long now = esp_timer_get_time() / 1000;
    unsigned long elapsed = now - enable_time;
    if (elapsed >= timeout_ms) return 0;
    return timeout_ms - elapsed;
}

std::string BluetoothManager::check_ota_failure_after_boot() {
    return ota_handler.check_ota_failure_after_boot();
}

void BluetoothManager::log(const char* format, ...) {
    // Stub - logging disabled in stub implementation
    // TODO: Re-enable when NimBLE headers are fixed
}
