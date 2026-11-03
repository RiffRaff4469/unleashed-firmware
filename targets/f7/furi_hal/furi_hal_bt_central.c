#include <furi_hal_bt_central.h>

bool furi_hal_bt_central_is_supported(void) {
    return gap_central_is_supported();
}

bool furi_hal_bt_scan_start(const GapScanParams* params, GapScanCallback callback, void* context) {
    return gap_scan_start(params, callback, context);
}

void furi_hal_bt_scan_stop(void) {
    gap_scan_stop();
}

bool furi_hal_bt_scan_is_active(void) {
    return gap_scan_is_active();
}

void furi_hal_bt_central_set_callback(GapCentralEventCallback callback, void* context) {
    gap_central_set_callback(callback, context);
}

bool furi_hal_bt_central_connect(GapAddressType address_type, const uint8_t* address) {
    return gap_central_connect(address_type, address);
}

bool furi_hal_bt_central_connect_cancel(void) {
    return gap_central_connect_cancel();
}

bool furi_hal_bt_central_disconnect(uint16_t conn_handle) {
    return gap_central_disconnect(conn_handle);
}

bool furi_hal_bt_central_exchange_mtu(uint16_t conn_handle) {
    return gap_central_exchange_mtu(conn_handle);
}

bool furi_hal_bt_central_discover_services(uint16_t conn_handle) {
    return gap_central_discover_services(conn_handle);
}

bool furi_hal_bt_central_discover_characteristics(
    uint16_t conn_handle,
    uint16_t start_handle,
    uint16_t end_handle) {
    return gap_central_discover_characteristics(conn_handle, start_handle, end_handle);
}

bool furi_hal_bt_central_discover_descriptors(
    uint16_t conn_handle,
    uint16_t value_handle,
    uint16_t end_handle) {
    return gap_central_discover_descriptors(conn_handle, value_handle, end_handle);
}

bool furi_hal_bt_central_read(uint16_t conn_handle, uint16_t handle) {
    return gap_central_read(conn_handle, handle);
}

bool furi_hal_bt_central_write(
    uint16_t conn_handle,
    uint16_t handle,
    const uint8_t* data,
    uint8_t len,
    bool with_response) {
    return gap_central_write(conn_handle, handle, data, len, with_response);
}

bool furi_hal_bt_central_subscribe(
    uint16_t conn_handle,
    uint16_t cccd_handle,
    bool notify,
    bool indicate) {
    return gap_central_subscribe(conn_handle, cccd_handle, notify, indicate);
}
