#include "gap_central.h"
#include "app_common.h"

#include <ble/ble.h>
#include <furi_hal_bt.h>
#include <furi.h>

#define TAG "BleCentral"

#define GAP_MS_TO_SCAN_UNITS(x) ((uint32_t)(x) * 8U / 5U) // 0.625 ms units
#define GAP_SCAN_UNITS_MIN      (0x0004U)
#define GAP_SCAN_UNITS_MAX      (0x4000U)

// Connection setup. Keep the interval at 30 ms or more: core2 needs the slack
// to grant core1 flash access (AN5289, 4.7).
#define GAP_CONNECT_SCAN_INTERVAL    (0x0060U) // 60 ms
#define GAP_CONNECT_SCAN_WINDOW      (0x0030U) // 30 ms
#define GAP_CONN_INTERVAL_MIN        (0x0018U) // 30 ms
#define GAP_CONN_INTERVAL_MAX        (0x0028U) // 50 ms
#define GAP_CONN_SUPERVISION_TIMEOUT (0x0190U) // 4 s

#define GAP_CENTRAL_PROC_TIMEOUT_ERROR       (0xFFU)
#define GAP_CENTRAL_ATT_FIND_INFO_FORMAT_128 (0x02U)

typedef struct {
    uint16_t conn_handle;
    uint16_t pending_read_handle;
} GapCentralLink;

typedef struct {
    FuriMutex* mutex;
    bool ready;
    bool scanning;
    bool connecting;
    GapScanCallback scan_callback;
    void* scan_context;
    GapCentralEventCallback callback;
    void* context;
    GapCentralLink links[CFG_BLE_NUM_LINK];
} GapCentral;

static GapCentral gap_central = {0};

static bool gap_central_acquire(void) {
    if(!gap_central.mutex) return false;
    furi_check(furi_mutex_acquire(gap_central.mutex, FuriWaitForever) == FuriStatusOk);
    if(!gap_central.ready) {
        furi_check(furi_mutex_release(gap_central.mutex) == FuriStatusOk);
        return false;
    }
    return true;
}

static void gap_central_release(void) {
    furi_check(furi_mutex_release(gap_central.mutex) == FuriStatusOk);
}

static GapCentralLink* gap_central_find_link(uint16_t conn_handle) {
    for(size_t i = 0; i < COUNT_OF(gap_central.links); i++) {
        if(gap_central.links[i].conn_handle == conn_handle) {
            return &gap_central.links[i];
        }
    }
    return NULL;
}

// Live link by handle, never a free slot
static GapCentralLink* gap_central_get_link(uint16_t conn_handle) {
    if(conn_handle == GAP_CENTRAL_INVALID_HANDLE) return NULL;
    return gap_central_find_link(conn_handle);
}

static void gap_central_emit(GapCentralEvent* event) {
    if(gap_central.callback) {
        gap_central.callback(event, gap_central.context);
    }
}

static void gap_central_set_uuid(GapUuid* uuid, const uint8_t* data, size_t len) {
    memset(uuid, 0, sizeof(GapUuid));
    uuid->type = (len == 16) ? GapUuidType128 : GapUuidType16;
    memcpy(uuid->uuid, data, len);
}

static uint16_t gap_central_get_u16(const uint8_t* data) {
    return data[0] | (data[1] << 8);
}

void gap_central_init(void) {
    if(!gap_central.mutex) {
        // Recursive: event callbacks are allowed to call back into the API
        gap_central.mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
    }
    furi_check(furi_mutex_acquire(gap_central.mutex, FuriWaitForever) == FuriStatusOk);
    for(size_t i = 0; i < COUNT_OF(gap_central.links); i++) {
        gap_central.links[i].conn_handle = GAP_CENTRAL_INVALID_HANDLE;
    }
    gap_central.scanning = false;
    gap_central.connecting = false;
    gap_central.ready = furi_hal_bt_get_radio_stack() == FuriHalBtStackFull;
    FURI_LOG_I(TAG, "Init, central role %s", gap_central.ready ? "available" : "unavailable");
    furi_check(furi_mutex_release(gap_central.mutex) == FuriStatusOk);
}

void gap_central_reset(void) {
    if(!gap_central_acquire()) return;

    for(size_t i = 0; i < COUNT_OF(gap_central.links); i++) {
        GapCentralLink* link = &gap_central.links[i];
        if(link->conn_handle != GAP_CENTRAL_INVALID_HANDLE) {
            GapCentralEvent event = {
                .type = GapCentralEventTypeDisconnected,
                .conn_handle = link->conn_handle,
                .data.disconnected.reason = HCI_CONNECTION_TERMINATED_BY_LOCAL_HOST_ERR_CODE,
            };
            link->conn_handle = GAP_CENTRAL_INVALID_HANDLE;
            gap_central_emit(&event);
        }
    }
    gap_central.scanning = false;
    gap_central.scan_callback = NULL;
    gap_central.scan_context = NULL;
    gap_central.connecting = false;
    gap_central.ready = false;

    gap_central_release();
}

bool gap_central_is_supported(void) {
    if(!gap_central_acquire()) return false;
    gap_central_release();
    return true;
}

bool gap_scan_start(const GapScanParams* params, GapScanCallback callback, void* context) {
    furi_check(params);
    furi_check(callback);
    if(!gap_central_acquire()) return false;

    bool success = false;
    do {
        if(gap_central.scanning || gap_central.connecting) break;

        uint32_t interval = GAP_MS_TO_SCAN_UNITS(params->interval_ms);
        uint32_t window = GAP_MS_TO_SCAN_UNITS(params->window_ms);
        if(window < GAP_SCAN_UNITS_MIN || interval > GAP_SCAN_UNITS_MAX || window > interval) {
            FURI_LOG_E(TAG, "Invalid scan timing");
            break;
        }

        gap_central.scan_callback = callback;
        gap_central.scan_context = context;
        tBleStatus status = aci_gap_start_observation_proc(
            interval,
            window,
            params->active ? HCI_SCAN_TYPE_ACTIVE : HCI_SCAN_TYPE_PASSIVE,
            GAP_PUBLIC_ADDR,
            params->filter_duplicates ? 0x01 : 0x00,
            0x00); // Accept all advertisements
        if(status) {
            FURI_LOG_E(TAG, "start_observation_proc failed %d", status);
            gap_central.scan_callback = NULL;
            gap_central.scan_context = NULL;
            break;
        }

        gap_central.scanning = true;
        success = true;
    } while(false);

    gap_central_release();
    return success;
}

void gap_scan_stop(void) {
    if(!gap_central_acquire()) return;

    if(gap_central.scanning) {
        tBleStatus status = aci_gap_terminate_gap_proc(GAP_OBSERVATION_PROC);
        if(status) {
            FURI_LOG_E(TAG, "terminate observation failed %d", status);
        }
        gap_central.scanning = false;
    }
    // No scan callbacks after this point: reports are dispatched under the same mutex
    gap_central.scan_callback = NULL;
    gap_central.scan_context = NULL;

    gap_central_release();
}

bool gap_scan_is_active(void) {
    if(!gap_central_acquire()) return false;
    bool active = gap_central.scanning;
    gap_central_release();
    return active;
}

void gap_central_set_callback(GapCentralEventCallback callback, void* context) {
    // Set even when the role is unavailable, so it survives radio restarts
    if(!gap_central.mutex) return;
    furi_check(furi_mutex_acquire(gap_central.mutex, FuriWaitForever) == FuriStatusOk);
    gap_central.callback = callback;
    gap_central.context = context;
    furi_check(furi_mutex_release(gap_central.mutex) == FuriStatusOk);
}

bool gap_central_connect(GapAddressType address_type, const uint8_t* address) {
    furi_check(address);
    if(!gap_central_acquire()) return false;

    bool success = false;
    do {
        // Only one GAP procedure at a time: scanning must be stopped first
        if(gap_central.connecting || gap_central.scanning) break;
        if(!gap_central_find_link(GAP_CENTRAL_INVALID_HANDLE)) break;

        tBleStatus status = aci_gap_create_connection(
            GAP_CONNECT_SCAN_INTERVAL,
            GAP_CONNECT_SCAN_WINDOW,
            address_type,
            address,
            GAP_PUBLIC_ADDR,
            GAP_CONN_INTERVAL_MIN,
            GAP_CONN_INTERVAL_MAX,
            0,
            GAP_CONN_SUPERVISION_TIMEOUT,
            0,
            0);
        if(status) {
            FURI_LOG_E(TAG, "create_connection failed %d", status);
            break;
        }

        gap_central.connecting = true;
        success = true;
    } while(false);

    gap_central_release();
    return success;
}

bool gap_central_connect_cancel(void) {
    if(!gap_central_acquire()) return false;

    bool success = false;
    if(gap_central.connecting) {
        // ConnectFailed is reported once the controller confirms
        tBleStatus status = aci_gap_terminate_gap_proc(GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC);
        if(status) {
            FURI_LOG_E(TAG, "terminate connection proc failed %d", status);
        } else {
            success = true;
        }
    }

    gap_central_release();
    return success;
}

bool gap_central_disconnect(uint16_t conn_handle) {
    if(!gap_central_acquire()) return false;

    bool success = false;
    if(gap_central_get_link(conn_handle)) {
        success = aci_gap_terminate(conn_handle, HCI_REMOTE_USER_TERMINATED_CONNECTION_ERR_CODE) ==
                  BLE_STATUS_SUCCESS;
    }

    gap_central_release();
    return success;
}

// Run a GATT client command on a central link
#define GAP_CENTRAL_LINK_COMMAND(conn_handle, command)                  \
    ({                                                                  \
        bool _success = false;                                          \
        if(gap_central_acquire()) {                                     \
            if(gap_central_get_link(conn_handle)) {                     \
                tBleStatus _status = (command);                         \
                if(_status) {                                           \
                    FURI_LOG_E(TAG, "%s failed %d", __func__, _status); \
                }                                                       \
                _success = _status == BLE_STATUS_SUCCESS;               \
            }                                                           \
            gap_central_release();                                      \
        }                                                               \
        _success;                                                       \
    })

bool gap_central_exchange_mtu(uint16_t conn_handle) {
    return GAP_CENTRAL_LINK_COMMAND(conn_handle, aci_gatt_exchange_config(conn_handle));
}

bool gap_central_discover_services(uint16_t conn_handle) {
    return GAP_CENTRAL_LINK_COMMAND(conn_handle, aci_gatt_disc_all_primary_services(conn_handle));
}

bool gap_central_discover_characteristics(
    uint16_t conn_handle,
    uint16_t start_handle,
    uint16_t end_handle) {
    return GAP_CENTRAL_LINK_COMMAND(
        conn_handle, aci_gatt_disc_all_char_of_service(conn_handle, start_handle, end_handle));
}

bool gap_central_discover_descriptors(
    uint16_t conn_handle,
    uint16_t value_handle,
    uint16_t end_handle) {
    return GAP_CENTRAL_LINK_COMMAND(
        conn_handle, aci_gatt_disc_all_char_desc(conn_handle, value_handle, end_handle));
}

bool gap_central_read(uint16_t conn_handle, uint16_t handle) {
    if(!gap_central_acquire()) return false;

    bool success = false;
    GapCentralLink* link = gap_central_get_link(conn_handle);
    if(link) {
        // The read response does not carry the handle, so remember it
        link->pending_read_handle = handle;
        tBleStatus status = aci_gatt_read_char_value(conn_handle, handle);
        if(status) {
            FURI_LOG_E(TAG, "read_char_value failed %d", status);
        }
        success = status == BLE_STATUS_SUCCESS;
    }

    gap_central_release();
    return success;
}

bool gap_central_write(
    uint16_t conn_handle,
    uint16_t handle,
    const uint8_t* data,
    uint8_t len,
    bool with_response) {
    furi_check(data || !len);
    if(with_response) {
        return GAP_CENTRAL_LINK_COMMAND(
            conn_handle, aci_gatt_write_char_value(conn_handle, handle, len, data));
    } else {
        return GAP_CENTRAL_LINK_COMMAND(
            conn_handle, aci_gatt_write_without_resp(conn_handle, handle, len, data));
    }
}

bool gap_central_subscribe(uint16_t conn_handle, uint16_t cccd_handle, bool notify, bool indicate) {
    uint8_t value[2] = {(notify ? 0x01 : 0x00) | (indicate ? 0x02 : 0x00), 0x00};
    return GAP_CENTRAL_LINK_COMMAND(
        conn_handle, aci_gatt_write_char_desc(conn_handle, cccd_handle, sizeof(value), value));
}

static void gap_central_process_adv_report(const uint8_t* data, size_t len) {
    if(!gap_central.scan_callback || len < 1) return;

    // Num_Reports, then per report: type, address type, address, data length, data, RSSI
    uint8_t num_reports = data[0];
    size_t offset = 1;
    for(uint8_t i = 0; i < num_reports; i++) {
        if(offset + 9 > len) break;
        GapScanReport report;
        report.type = data[offset];
        report.address_type = data[offset + 1];
        memcpy(report.address, &data[offset + 2], sizeof(report.address));
        report.data_len = data[offset + 8];
        offset += 9;
        if(report.data_len > GAP_SCAN_MAX_DATA_SIZE || offset + report.data_len + 1 > len) break;
        memcpy(report.data, &data[offset], report.data_len);
        offset += report.data_len;
        report.rssi = (int8_t)data[offset];
        offset += 1;
        gap_central.scan_callback(&report, gap_central.scan_context);
    }
}

static void gap_central_process_connection_complete(
    uint8_t status,
    uint16_t conn_handle,
    uint8_t address_type,
    const uint8_t* address,
    uint16_t conn_interval) {
    bool was_connecting = gap_central.connecting;
    gap_central.connecting = false;

    if(status) {
        FURI_LOG_W(TAG, "Connection failed %02X", status);
        if(was_connecting) {
            GapCentralEvent event = {
                .type = GapCentralEventTypeConnectFailed,
                .conn_handle = GAP_CENTRAL_INVALID_HANDLE,
                .data.connect_failed.status = status,
            };
            gap_central_emit(&event);
        }
        return;
    }

    GapCentralLink* link = gap_central_find_link(GAP_CENTRAL_INVALID_HANDLE);
    if(!link) {
        // The controller never gives us more links than configured, but be safe
        FURI_LOG_E(TAG, "No free link slot, dropping %04X", conn_handle);
        aci_gap_terminate(conn_handle, HCI_REMOTE_USER_TERMINATED_CONNECTION_ERR_CODE);
        return;
    }
    link->conn_handle = conn_handle;
    link->pending_read_handle = 0;
    FURI_LOG_I(TAG, "Connected %04X", conn_handle);

    GapCentralEvent event = {
        .type = GapCentralEventTypeConnected,
        .conn_handle = conn_handle,
        .data.connected.address_type = address_type,
        .data.connected.conn_interval = conn_interval,
    };
    memcpy(event.data.connected.address, address, GAP_CENTRAL_MAC_ADDR_SIZE);
    gap_central_emit(&event);
}

static bool gap_central_process_le_meta(const evt_le_meta_event* meta_evt, size_t len) {
    switch(meta_evt->subevent) {
    case HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE:
        gap_central_process_adv_report(meta_evt->data, len);
        return true;

    case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE: {
        const hci_le_connection_complete_event_rp0* evt = (const void*)meta_evt->data;
        // Peripheral GAP does not check the status, so failed attempts are ours too
        if(evt->Role != HCI_ROLE_CENTRAL && evt->Status == BLE_STATUS_SUCCESS) return false;
        gap_central_process_connection_complete(
            evt->Status,
            evt->Connection_Handle,
            evt->Peer_Address_Type,
            evt->Peer_Address,
            evt->Conn_Interval);
        return true;
    }

    case HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE: {
        const hci_le_enhanced_connection_complete_event_rp0* evt = (const void*)meta_evt->data;
        if(evt->Role != HCI_ROLE_CENTRAL && evt->Status == BLE_STATUS_SUCCESS) return false;
        gap_central_process_connection_complete(
            evt->Status,
            evt->Connection_Handle,
            evt->Peer_Address_Type,
            evt->Peer_Address,
            evt->Conn_Interval);
        return true;
    }

    case HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE:
    case HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE: {
        // Both start with Status, Connection_Handle
        uint16_t conn_handle = gap_central_get_u16(&meta_evt->data[1]);
        return gap_central_get_link(conn_handle) != NULL;
    }

    default:
        return false;
    }
}

static bool gap_central_process_vendor(const evt_blecore_aci* blue_evt) {
    if(blue_evt->ecode == ACI_GAP_PROC_COMPLETE_VSEVT_CODE) {
        const aci_gap_proc_complete_event_rp0* evt = (const void*)blue_evt->data;
        if(evt->Procedure_Code == GAP_OBSERVATION_PROC) {
            gap_central.scanning = false;
            return true;
        } else if(evt->Procedure_Code == GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC) {
            if(evt->Status && gap_central.connecting) {
                gap_central_process_connection_complete(evt->Status, 0, 0, NULL, 0);
            }
            return true;
        }
        return false;
    }

    // Everything else we handle starts with Connection_Handle
    switch(blue_evt->ecode) {
    case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE:
    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE:
    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE:
    case ACI_ATT_READ_RESP_VSEVT_CODE:
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE:
    case ACI_GATT_NOTIFICATION_VSEVT_CODE:
    case ACI_GATT_INDICATION_VSEVT_CODE:
    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE:
    case ACI_GATT_PROC_TIMEOUT_VSEVT_CODE:
    case ACI_GATT_ERROR_RESP_VSEVT_CODE:
    case ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE:
    case ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE:
    case ACI_GAP_PASS_KEY_REQ_VSEVT_CODE:
    case ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE:
    case ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE:
    case ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE:
    case ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE:
        break;
    default:
        return false;
    }

    uint16_t conn_handle = gap_central_get_u16(blue_evt->data);
    GapCentralLink* link = gap_central_get_link(conn_handle);
    if(!link) return false;

    GapCentralEvent event = {.conn_handle = conn_handle};

    switch(blue_evt->ecode) {
    case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE: {
        const aci_att_exchange_mtu_resp_event_rp0* evt = (const void*)blue_evt->data;
        event.type = GapCentralEventTypeMtuExchanged;
        event.data.mtu.mtu = evt->Server_RX_MTU;
        gap_central_emit(&event);
    } break;

    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
        // Primary services: start handle, end handle, UUID
        const aci_att_read_by_group_type_resp_event_rp0* evt = (const void*)blue_evt->data;
        uint8_t item_len = evt->Attribute_Data_Length;
        if(item_len != 6 && item_len != 20) break;
        event.type = GapCentralEventTypeServiceFound;
        for(size_t i = 0; i + item_len <= evt->Data_Length; i += item_len) {
            const uint8_t* item = &evt->Attribute_Data_List[i];
            event.data.service.start_handle = gap_central_get_u16(item);
            event.data.service.end_handle = gap_central_get_u16(item + 2);
            gap_central_set_uuid(&event.data.service.uuid, item + 4, item_len - 4);
            gap_central_emit(&event);
        }
    } break;

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
        // Characteristic declarations: handle, properties, value handle, UUID
        const aci_att_read_by_type_resp_event_rp0* evt = (const void*)blue_evt->data;
        uint8_t item_len = evt->Handle_Value_Pair_Length;
        if(item_len != 7 && item_len != 21) break;
        event.type = GapCentralEventTypeCharacteristicFound;
        for(size_t i = 0; i + item_len <= evt->Data_Length; i += item_len) {
            const uint8_t* item = &evt->Handle_Value_Pair_Data[i];
            event.data.characteristic.decl_handle = gap_central_get_u16(item);
            event.data.characteristic.properties = item[2];
            event.data.characteristic.value_handle = gap_central_get_u16(item + 3);
            gap_central_set_uuid(&event.data.characteristic.uuid, item + 5, item_len - 5);
            gap_central_emit(&event);
        }
    } break;

    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE: {
        // Descriptors: handle, UUID
        const aci_att_find_info_resp_event_rp0* evt = (const void*)blue_evt->data;
        uint8_t item_len = (evt->Format == GAP_CENTRAL_ATT_FIND_INFO_FORMAT_128) ? 18 : 4;
        event.type = GapCentralEventTypeDescriptorFound;
        for(size_t i = 0; i + item_len <= evt->Event_Data_Length; i += item_len) {
            const uint8_t* item = &evt->Handle_UUID_Pair[i];
            event.data.descriptor.handle = gap_central_get_u16(item);
            gap_central_set_uuid(&event.data.descriptor.uuid, item + 2, item_len - 2);
            gap_central_emit(&event);
        }
    } break;

    case ACI_ATT_READ_RESP_VSEVT_CODE: {
        const aci_att_read_resp_event_rp0* evt = (const void*)blue_evt->data;
        event.type = GapCentralEventTypeReadResponse;
        event.data.value.handle = link->pending_read_handle;
        event.data.value.len = evt->Event_Data_Length;
        event.data.value.data = evt->Attribute_Value;
        gap_central_emit(&event);
    } break;

    case ACI_GATT_NOTIFICATION_VSEVT_CODE:
    case ACI_GATT_INDICATION_VSEVT_CODE: {
        // Same layout for both
        const aci_gatt_notification_event_rp0* evt = (const void*)blue_evt->data;
        bool is_indication = blue_evt->ecode == ACI_GATT_INDICATION_VSEVT_CODE;
        event.type = is_indication ? GapCentralEventTypeIndication :
                                     GapCentralEventTypeNotification;
        event.data.value.handle = evt->Attribute_Handle;
        event.data.value.len = evt->Attribute_Value_Length;
        event.data.value.data = evt->Attribute_Value;
        gap_central_emit(&event);
        if(is_indication) {
            aci_gatt_confirm_indication(conn_handle);
        }
    } break;

    case ACI_GATT_ERROR_RESP_VSEVT_CODE: {
        const aci_gatt_error_resp_event_rp0* evt = (const void*)blue_evt->data;
        event.type = GapCentralEventTypeAttError;
        event.data.att_error.req_opcode = evt->Req_Opcode;
        event.data.att_error.handle = evt->Attribute_Handle;
        event.data.att_error.error_code = evt->Error_Code;
        gap_central_emit(&event);
    } break;

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        const aci_gatt_proc_complete_event_rp0* evt = (const void*)blue_evt->data;
        event.type = GapCentralEventTypeProcedureComplete;
        event.data.procedure_complete.error_code = evt->Error_Code;
        gap_central_emit(&event);
    } break;

    case ACI_GATT_PROC_TIMEOUT_VSEVT_CODE:
        FURI_LOG_W(TAG, "GATT procedure timeout on %04X", conn_handle);
        event.type = GapCentralEventTypeProcedureComplete;
        event.data.procedure_complete.error_code = GAP_CENTRAL_PROC_TIMEOUT_ERROR;
        gap_central_emit(&event);
        break;

    case ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE: {
        // As central we must answer the peripheral's parameter request
        const aci_l2cap_connection_update_req_event_rp0* evt = (const void*)blue_evt->data;
        bool accept = evt->Interval_Max >= GAP_CONN_INTERVAL_MIN;
        uint16_t interval_min = MAX(evt->Interval_Min, GAP_CONN_INTERVAL_MIN);
        FURI_LOG_I(
            TAG,
            "Peripheral asks for interval %u-%u, %s",
            evt->Interval_Min,
            evt->Interval_Max,
            accept ? "accepting" : "rejecting");
        aci_l2cap_connection_parameter_update_resp(
            conn_handle,
            accept ? interval_min : evt->Interval_Min,
            evt->Interval_Max,
            evt->Latency,
            evt->Timeout_Multiplier,
            0,
            0,
            evt->Identifier,
            accept ? 0x01 : 0x00);
    } break;

    default:
        // Pairing is not supported on central links yet. Swallow these so the
        // peripheral GAP does not act on a connection it does not own.
        FURI_LOG_D(TAG, "Ignoring security event %04X on %04X", blue_evt->ecode, conn_handle);
        break;
    }

    return true;
}

bool gap_central_process_event(void* pckt) {
    if(!gap_central_acquire()) return false;

    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;
    bool consumed = false;

    switch(event_pckt->evt) {
    case HCI_DISCONNECTION_COMPLETE_EVT_CODE: {
        const hci_disconnection_complete_event_rp0* evt = (const void*)event_pckt->data;
        GapCentralLink* link = gap_central_get_link(evt->Connection_Handle);
        if(link) {
            FURI_LOG_I(TAG, "Disconnected %04X, reason %02X", evt->Connection_Handle, evt->Reason);
            link->conn_handle = GAP_CENTRAL_INVALID_HANDLE;
            GapCentralEvent event = {
                .type = GapCentralEventTypeDisconnected,
                .conn_handle = evt->Connection_Handle,
                .data.disconnected.reason = evt->Reason,
            };
            gap_central_emit(&event);
            consumed = true;
        }
    } break;

    case HCI_LE_META_EVT_CODE:
        if(event_pckt->plen > 0) {
            consumed = gap_central_process_le_meta(
                (const evt_le_meta_event*)event_pckt->data, event_pckt->plen - 1);
        }
        break;

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE:
        consumed = gap_central_process_vendor((const evt_blecore_aci*)event_pckt->data);
        break;

    default:
        break;
    }

    gap_central_release();
    return consumed;
}
