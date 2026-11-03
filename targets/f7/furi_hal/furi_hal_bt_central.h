/**
 * @file furi_hal_bt_central.h
 * BLE observer (scan) and central (GATT client) HAL API
 *
 * Requires the full BLE stack on core2. With the light stack every call fails
 * and furi_hal_bt_central_is_supported() returns false.
 *
 * Callbacks run on the BLE event thread: do not block in them, post to your own
 * queue instead. Calling back into this API from a callback is allowed.
 * After furi_hal_bt_scan_stop() or furi_hal_bt_central_set_callback(NULL, NULL)
 * return, the old callback is guaranteed not to be called again.
 *
 * GATT client procedures are asynchronous and one at a time per connection:
 * start one, then wait for GapCentralEventTypeProcedureComplete before the next.
 * Write without response and notifications do not use a procedure.
 *
 * The radio is owned by the Bt service. If it restarts the stack (profile
 * switch, BT turned off), scanning stops and every central link gets a
 * GapCentralEventTypeDisconnected event.
 */

#pragma once

#include <gap_central.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Check that the observer and central roles are available
 *
 * @return     true if core2 runs the full stack and the radio is up
 */
bool furi_hal_bt_central_is_supported(void);

/** Start scanning
 *
 * Only one scan at a time, and not while a connection is being established.
 *
 * @param      params    scan parameters
 * @param      callback  called for every advertising report
 * @param      context   callback context
 *
 * @return     true on success
 */
bool furi_hal_bt_scan_start(const GapScanParams* params, GapScanCallback callback, void* context);

/** Stop scanning */
void furi_hal_bt_scan_stop(void);

/** Check if scanning
 *
 * @return     true if a scan is running
 */
bool furi_hal_bt_scan_is_active(void);

/** Set the callback for central link events
 *
 * Must be set before connecting. Survives radio restarts.
 *
 * @param      callback  event callback, NULL to remove
 * @param      context   callback context
 */
void furi_hal_bt_central_set_callback(GapCentralEventCallback callback, void* context);

/** Connect to a peripheral
 *
 * Stop scanning first. The result arrives as GapCentralEventTypeConnected or
 * GapCentralEventTypeConnectFailed. There is no timeout: use
 * furi_hal_bt_central_connect_cancel() to give up.
 *
 * @param      address_type  address type, as seen in the advertising report
 * @param      address       6 byte address, as seen in the advertising report
 *
 * @return     true if the connection attempt started
 */
bool furi_hal_bt_central_connect(GapAddressType address_type, const uint8_t* address);

/** Cancel a pending connection attempt
 *
 * @return     true if cancelled, GapCentralEventTypeConnectFailed follows
 */
bool furi_hal_bt_central_connect_cancel(void);

/** Disconnect a central link
 *
 * @param      conn_handle  connection handle
 *
 * @return     true if disconnection started, GapCentralEventTypeDisconnected follows
 */
bool furi_hal_bt_central_disconnect(uint16_t conn_handle);

/** Exchange ATT MTU (GATT procedure)
 *
 * @param      conn_handle  connection handle
 *
 * @return     true if started, GapCentralEventTypeMtuExchanged follows
 */
bool furi_hal_bt_central_exchange_mtu(uint16_t conn_handle);

/** Discover all primary services (GATT procedure)
 *
 * @param      conn_handle  connection handle
 *
 * @return     true if started, GapCentralEventTypeServiceFound events follow
 */
bool furi_hal_bt_central_discover_services(uint16_t conn_handle);

/** Discover characteristics in a handle range (GATT procedure)
 *
 * @param      conn_handle   connection handle
 * @param      start_handle  service start handle
 * @param      end_handle    service end handle
 *
 * @return     true if started, GapCentralEventTypeCharacteristicFound events follow
 */
bool furi_hal_bt_central_discover_characteristics(
    uint16_t conn_handle,
    uint16_t start_handle,
    uint16_t end_handle);

/** Discover descriptors of a characteristic (GATT procedure)
 *
 * Use it to find the CCCD (UUID 0x2902) before subscribing.
 *
 * @param      conn_handle   connection handle
 * @param      value_handle  characteristic value handle
 * @param      end_handle    last handle of the characteristic (the next declaration
 *                           handle - 1, or the service end handle)
 *
 * @return     true if started, GapCentralEventTypeDescriptorFound events follow
 */
bool furi_hal_bt_central_discover_descriptors(
    uint16_t conn_handle,
    uint16_t value_handle,
    uint16_t end_handle);

/** Read an attribute (GATT procedure)
 *
 * Values longer than MTU - 1 are truncated.
 *
 * @param      conn_handle  connection handle
 * @param      handle       attribute handle
 *
 * @return     true if started, GapCentralEventTypeReadResponse follows
 */
bool furi_hal_bt_central_read(uint16_t conn_handle, uint16_t handle);

/** Write an attribute
 *
 * @param      conn_handle    connection handle
 * @param      handle         attribute handle
 * @param      data           value
 * @param      len            value length, at most MTU - 3
 * @param      with_response  true: GATT procedure with completion event,
 *                            false: write command, no event
 *
 * @return     true on success
 */
bool furi_hal_bt_central_write(
    uint16_t conn_handle,
    uint16_t handle,
    const uint8_t* data,
    uint8_t len,
    bool with_response);

/** Enable or disable notifications and indications (GATT procedure)
 *
 * @param      conn_handle  connection handle
 * @param      cccd_handle  Client Characteristic Configuration Descriptor handle
 * @param      notify       enable notifications
 * @param      indicate     enable indications
 *
 * @return     true if started, GapCentralEventTypeProcedureComplete follows
 */
bool furi_hal_bt_central_subscribe(
    uint16_t conn_handle,
    uint16_t cccd_handle,
    bool notify,
    bool indicate);

#ifdef __cplusplus
}
#endif
