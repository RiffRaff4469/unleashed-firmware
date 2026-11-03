#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "extra_beacon.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Observer (scan) and central (GATT client) roles.
 * Only available with the full BLE stack on core2.
 * Not to be used directly, but through furi_hal_bt_scan_* and furi_hal_bt_central_* APIs.
 */

#define GAP_SCAN_MAX_DATA_SIZE     (31)
#define GAP_CENTRAL_MAC_ADDR_SIZE  (6)
#define GAP_CENTRAL_INVALID_HANDLE (0xFFFF)

/** Advertising report event type, as reported by the controller */
typedef enum {
    GapScanReportTypeAdvInd = 0x00, /**< Connectable, scannable */
    GapScanReportTypeAdvDirectInd = 0x01, /**< Connectable, directed */
    GapScanReportTypeAdvScanInd = 0x02, /**< Scannable, not connectable */
    GapScanReportTypeAdvNonConnInd = 0x03, /**< Neither connectable nor scannable */
    GapScanReportTypeScanRsp = 0x04, /**< Scan response (active scan only) */
} GapScanReportType;

typedef struct {
    GapScanReportType type;
    GapAddressType address_type;
    uint8_t address[GAP_CENTRAL_MAC_ADDR_SIZE]; /**< Little endian, as on air */
    int8_t rssi; /**< dBm, 127 if not available */
    uint8_t data_len;
    uint8_t data[GAP_SCAN_MAX_DATA_SIZE];
} GapScanReport;

/** Called from the BLE event thread for every advertising report. Must not block. */
typedef void (*GapScanCallback)(const GapScanReport* report, void* context);

typedef struct {
    uint16_t interval_ms; /**< 3..10240 */
    uint16_t window_ms; /**< 3..interval_ms */
    bool active; /**< Send scan requests, so scan responses are reported too */
    bool filter_duplicates; /**< Let the controller drop repeated reports */
} GapScanParams;

typedef enum {
    GapUuidType16 = 0x01,
    GapUuidType128 = 0x02,
} GapUuidType;

typedef struct {
    GapUuidType type;
    uint8_t uuid[16]; /**< Little endian. Only the first 2 bytes are used for 16 bit UUIDs */
} GapUuid;

/** Characteristic properties bit mask */
typedef enum {
    GapCharPropertyBroadcast = 0x01,
    GapCharPropertyRead = 0x02,
    GapCharPropertyWriteWithoutResponse = 0x04,
    GapCharPropertyWrite = 0x08,
    GapCharPropertyNotify = 0x10,
    GapCharPropertyIndicate = 0x20,
    GapCharPropertySignedWrite = 0x40,
    GapCharPropertyExtended = 0x80,
} GapCharProperty;

typedef enum {
    GapCentralEventTypeConnected,
    GapCentralEventTypeConnectFailed,
    GapCentralEventTypeDisconnected,
    GapCentralEventTypeMtuExchanged,
    GapCentralEventTypeServiceFound,
    GapCentralEventTypeCharacteristicFound,
    GapCentralEventTypeDescriptorFound,
    GapCentralEventTypeReadResponse,
    GapCentralEventTypeNotification,
    GapCentralEventTypeIndication,
    GapCentralEventTypeAttError,
    GapCentralEventTypeProcedureComplete,
} GapCentralEventType;

typedef struct {
    GapCentralEventType type;
    uint16_t conn_handle;
    union {
        struct {
            GapAddressType address_type;
            uint8_t address[GAP_CENTRAL_MAC_ADDR_SIZE];
            uint16_t conn_interval; /**< 1.25 ms units */
        } connected;
        struct {
            uint8_t status; /**< HCI status code */
        } connect_failed;
        struct {
            uint8_t reason; /**< HCI reason code */
        } disconnected;
        struct {
            uint16_t mtu; /**< Server RX MTU */
        } mtu;
        struct {
            uint16_t start_handle;
            uint16_t end_handle;
            GapUuid uuid;
        } service;
        struct {
            uint16_t decl_handle;
            uint16_t value_handle;
            uint8_t properties; /**< GapCharProperty bit mask */
            GapUuid uuid;
        } characteristic;
        struct {
            uint16_t handle;
            GapUuid uuid;
        } descriptor;
        struct {
            uint16_t handle;
            uint16_t len;
            const uint8_t* data; /**< Valid only during the callback */
        } value;
        struct {
            uint8_t req_opcode;
            uint16_t handle;
            uint8_t error_code; /**< ATT error code */
        } att_error;
        struct {
            uint8_t error_code; /**< 0 on success */
        } procedure_complete;
    } data;
} GapCentralEvent;

/** Called from the BLE event thread. Must not block. */
typedef void (*GapCentralEventCallback)(const GapCentralEvent* event, void* context);

bool gap_central_is_supported(void);

bool gap_scan_start(const GapScanParams* params, GapScanCallback callback, void* context);

void gap_scan_stop(void);

bool gap_scan_is_active(void);

void gap_central_set_callback(GapCentralEventCallback callback, void* context);

bool gap_central_connect(GapAddressType address_type, const uint8_t* address);

bool gap_central_connect_cancel(void);

bool gap_central_disconnect(uint16_t conn_handle);

bool gap_central_exchange_mtu(uint16_t conn_handle);

bool gap_central_discover_services(uint16_t conn_handle);

bool gap_central_discover_characteristics(
    uint16_t conn_handle,
    uint16_t start_handle,
    uint16_t end_handle);

bool gap_central_discover_descriptors(
    uint16_t conn_handle,
    uint16_t value_handle,
    uint16_t end_handle);

bool gap_central_read(uint16_t conn_handle, uint16_t handle);

bool gap_central_write(
    uint16_t conn_handle,
    uint16_t handle,
    const uint8_t* data,
    uint8_t len,
    bool with_response);

bool gap_central_subscribe(uint16_t conn_handle, uint16_t cccd_handle, bool notify, bool indicate);

/* Glue internals */

/** Enable the roles if core2 runs the full stack. Called on GAP init. */
void gap_central_init(void);

/** Route an HCI event. Returns true if it belonged to the observer/central role. */
bool gap_central_process_event(void* pckt);

/** Drop scan and connection state, emitting Disconnected for live links. Called on GAP stop. */
void gap_central_reset(void);

#ifdef __cplusplus
}
#endif
