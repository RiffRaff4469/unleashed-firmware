# BLE scanning and central (GATT client) API {#ble_central}

This fork runs the **full** STM32WB BLE stack on core2 instead of the light one, and
exposes two roles the light stack doesn't have:

- **Observer:** scan for advertisements.
- **Central / GATT client:** connect to other BLE devices, discover their services,
  read, write, and subscribe to notifications.

The existing peripheral role (phone app, BLE remote, serial) keeps working at the
same time.

API header: `targets/f7/furi_hal/furi_hal_bt_central.h`. Types: `targets/f7/ble_glue/gap_central.h`.

## Requirements

| | |
|---|---|
| Firmware | this fork, **API 88.20** or newer |
| Radio stack | full, `radio.stack.type : 1` in `info device` (light is 3) |
| Target | f7 only |
| Bluetooth | on (Settings → Bluetooth) |

Always check at runtime. With the light stack, or with Bluetooth off, every call
fails and this returns false:

```c
#include <furi_hal_bt_central.h>

if(!furi_hal_bt_central_is_supported()) {
    // show "full BLE stack needed"
}
```

Build FAPs against the 88.20 SDK. An app built against it won't load on stock
88.19 firmware, but older 88.x apps still load on this firmware.

## How it fits together

- **The Bt service owns the radio.** You share it: the phone can stay connected while
  you scan or connect. If the Bt service restarts the stack (profile switch, Bluetooth
  toggled), scans stop and every central link gets a `Disconnected` event with reason
  `0x16`.
- **Two links in total** (`CFG_BLE_NUM_LINK`): normally one for the phone and one for
  your central connection. With no phone connected, two central links should work
  (untested).
- **Callbacks run on the BLE event thread.** Don't block in them. Copy what you need
  into your own `FuriMessageQueue` and handle it on your app thread. Calling the API
  from inside a callback is allowed.
- **Pointers in events are only valid during the callback.** That includes
  `event->data.value.data` (read responses, notifications). Copy the bytes.
- **Clean shutdown is guaranteed.** Once `furi_hal_bt_scan_stop()` or
  `furi_hal_bt_central_set_callback(NULL, NULL)` returns, the old callback will never
  run again, so it's safe to free its context.

## Scanning

```c
static void scan_callback(const GapScanReport* report, void* context) {
    FuriMessageQueue* queue = context;
    furi_message_queue_put(queue, report, 0); // drop reports if the queue is full
}

GapScanParams params = {
    .interval_ms = 100,
    .window_ms = 50,
    .active = true, // also get scan responses (often carry the device name)
    .filter_duplicates = false, // true: controller drops repeats, but RSSI stops updating
};
FuriMessageQueue* queue = furi_message_queue_alloc(16, sizeof(GapScanReport));
if(!furi_hal_bt_scan_start(&params, scan_callback, queue)) {
    // already scanning, a connection attempt is pending, bad timing, or not supported
}
// ...
furi_hal_bt_scan_stop();
```

`interval_ms` is 3–10240 and `window_ms` 3–`interval_ms`. A window equal to the
interval scans continuously, which finds devices faster but uses more power.

Each `GapScanReport` holds:

| Field | Meaning |
|---|---|
| `type` | `GapScanReportTypeAdvInd` / `AdvDirectInd` are connectable. `ScanRsp` is the scan response to an earlier advertisement (active scan only). |
| `address_type`, `address[6]` | Pass both unchanged to `furi_hal_bt_central_connect()`. Bytes are **little endian** (on-air order): print `address[5]` first. Random addresses can change over time. |
| `rssi` | dBm. 127 means not available. |
| `data`, `data_len` | Raw advertising data, at most 31 bytes (legacy advertising only). |

Advertising data is a list of `[length][type][payload]` structures. For example, to
get the device name:

```c
static bool adv_find_name(const GapScanReport* report, char* name, size_t name_size) {
    for(size_t i = 0; i + 1 < report->data_len;) {
        uint8_t len = report->data[i];
        if(len == 0 || i + 1 + len > report->data_len) break;
        uint8_t ad_type = report->data[i + 1];
        if(ad_type == 0x09 || ad_type == 0x08) { // complete or shortened local name
            size_t n = MIN((size_t)len - 1, name_size - 1);
            memcpy(name, &report->data[i + 2], n);
            name[n] = '\0';
            return true;
        }
        i += 1 + len;
    }
    return false;
}
```

The name is often only in the scan response, so use active scanning and merge reports
by address.

## Connecting

```c
furi_hal_bt_central_set_callback(central_callback, app); // before connecting
furi_hal_bt_scan_stop(); // required: no scan during a connection attempt
if(!furi_hal_bt_central_connect(report.address_type, report.address)) {
    // busy, no free link, or not supported
}
```

The result arrives as an event:

- **`Connected`:** `event->conn_handle` identifies the link from now on.
  `data.connected` repeats the address and gives the interval (1.25 ms units).
- **`ConnectFailed`:** `data.connect_failed.status` is an HCI code. `conn_handle` is
  `GAP_CENTRAL_INVALID_HANDLE`.

There is no timeout. Run your own timer and call `furi_hal_bt_central_connect_cancel()`,
which is followed by `ConnectFailed`.

`furi_hal_bt_central_disconnect(conn_handle)` ends a link. `Disconnected` follows with
an HCI reason:

| Reason | Meaning |
|---|---|
| `0x08` | Supervision timeout: out of range or the other side vanished |
| `0x13` | Remote side disconnected |
| `0x16` | You disconnected, or the Bt service restarted the radio |
| `0x3E` | Connection failed to be established |

**Retry once on `0x3E`.** On real devices (iPhone, Govee lights) about half of first
attempts drop within ~200 ms with this reason and the next attempt works. Phone BLE
stacks retry this silently.

Connection parameters are fixed at a 30–50 ms interval, latency 0 and a 4 s
supervision timeout. If the peripheral asks for an interval below 30 ms, the request
is rejected (core2 needs the slack for flash access). The link normally stays up anyway.

## GATT client

**Rule: one procedure at a time per connection.** Start one, then wait for
`ProcedureComplete` before starting the next. A call made while another procedure is
running fails.

| Call | Events, then `ProcedureComplete` |
|---|---|
| `furi_hal_bt_central_exchange_mtu(h)` | `MtuExchanged` (`data.mtu.mtu`, the server's RX MTU) |
| `furi_hal_bt_central_discover_services(h)` | `ServiceFound` × n (`start_handle`, `end_handle`, `uuid`) |
| `furi_hal_bt_central_discover_characteristics(h, start, end)` | `CharacteristicFound` × n (`decl_handle`, `value_handle`, `properties`, `uuid`) |
| `furi_hal_bt_central_discover_descriptors(h, value_handle, end)` | `DescriptorFound` × n (`handle`, `uuid`) |
| `furi_hal_bt_central_read(h, handle)` | `ReadResponse` (`data.value`) |
| `furi_hal_bt_central_write(h, handle, data, len, true)` | nothing but `ProcedureComplete` |
| `furi_hal_bt_central_subscribe(h, cccd, notify, indicate)` | nothing but `ProcedureComplete` |

These don't use a procedure and can run at any time:

- `furi_hal_bt_central_write(h, handle, data, len, false)` is a write command: no event
  and no acknowledgement.
- `Notification` and `Indication` events arrive whenever the server sends them.
  Indications are confirmed for you.

**`ProcedureComplete.error_code`:** 0 on success; non-zero means the procedure failed.
`0xFF` means the GATT procedure timed out, and the link should be treated as dead.

**`AttError`:** the server refused a request (`req_opcode`, `handle`, `error_code`).
A `ProcedureComplete` still follows. Common codes:

| Code | Meaning |
|---|---|
| `0x0A` | Attribute not found. **Normal** at the end of every discovery: it's how the server says "nothing more in this range". |
| `0x05` / `0x0F` | Insufficient authentication / encryption: the characteristic needs pairing, which this API doesn't support yet. iPhones do this for most notify characteristics. |
| `0x02` / `0x03` | Read / write not permitted |
| `0x06` | Request not supported |

### A typical session

1. `Connected` → `exchange_mtu`. Most devices accept 247 or more; the Flipper offers 414.
2. `ProcedureComplete` → `discover_services`. Store each service's handle range.
3. For each service, `discover_characteristics(start, end)`, one at a time.
4. To subscribe to a characteristic:
   1. Find its CCCD (UUID `0x2902`) with `discover_descriptors(value_handle, end)`.
      `end` is the next characteristic's `decl_handle - 1`, or the service's `end_handle`
      for the last one.
   2. `subscribe(cccd_handle, true, false)`.

UUIDs are little endian like everything on air. For a 16-bit UUID:
`uuid.type == GapUuidType16 && (uuid.uuid[0] | (uuid.uuid[1] << 8)) == 0x2A00`.

Size limits:

- **Reads** return up to MTU − 1 bytes. Long (blob) reads aren't supported.
- **Writes** take up to MTU − 3 bytes, and `len` is a `uint8_t`.

### Event handling skeleton

```c
typedef struct {
    GapCentralEvent event;
    uint8_t value[64]; // copy of event.data.value.data
} CentralMessage;

static void central_callback(const GapCentralEvent* event, void* context) {
    App* app = context;
    CentralMessage msg = {.event = *event};
    if(event->type == GapCentralEventTypeReadResponse ||
       event->type == GapCentralEventTypeNotification ||
       event->type == GapCentralEventTypeIndication) {
        uint16_t len = MIN(event->data.value.len, sizeof(msg.value));
        memcpy(msg.value, event->data.value.data, len);
        msg.event.data.value.len = len;
        msg.event.data.value.data = NULL; // the original pointer dies after we return
    }
    // Discovery can burst many events: wait a little rather than drop them
    furi_message_queue_put(app->central_queue, &msg, 10);
}
```

### Shutting down

```c
furi_hal_bt_scan_stop();
if(conn_handle != GAP_CENTRAL_INVALID_HANDLE) furi_hal_bt_central_disconnect(conn_handle);
furi_hal_bt_central_set_callback(NULL, NULL); // after this, no callback touches app
```

## Limits

- **No pairing or encryption on central links.** Security requests from the
  peripheral are ignored. Characteristics that need it fail with ATT `0x05`/`0x0F`.
- Legacy advertising only (31-byte payloads). No extended advertising, no periodic
  advertising.
- Two links in total, shared with the phone connection.
- Fixed connection parameters (30–50 ms). No RSSI or PHY reporting on connections.
- No discovery by UUID, long reads or long writes. Discover everything and filter.
- `furi_hal_bt_central_set_callback()` only sticks once the Bt service has started the
  radio. That's always true by the time an app runs, unless Bluetooth is off.

## Tested on hardware

Verified on 2026-11-03 against a Govee H617A light and an iPhone: scanning, connect,
MTU exchange (247 / 414), service and characteristic discovery, Device Name read, and
subscribing to notifications (Govee accepted; iPhone refused with `0x05`, pairing
required). This all ran while the Bt service kept its own peripheral role up.

## For firmware developers

| File | Role |
|---|---|
| `targets/f7/ble_glue/gap_central.c` | State, ACI calls and event parsing |
| `targets/f7/ble_glue/gap_central.h` | Public types and glue API (`gap_scan_*`, `gap_central_*`) |
| `targets/f7/furi_hal/furi_hal_bt_central.{c,h}` | Exported HAL wrappers (`+` in `api_symbols.csv`) |
| `targets/f7/ble_glue/gap.c` | `aci_gap_init` adds central + observer roles on the full stack only |
| `targets/f7/ble_glue/furi_ble/event_dispatcher.c` | Calls `gap_central_process_event()` before any service handler |

**Event routing.** `gap_central_process_event()` runs before the GATT service handlers
and the peripheral GAP, and claims:

- advertising reports;
- connection completes in the central role, plus *all* failed connection completes
  (the peripheral GAP doesn't check status);
- every event carrying the handle of a central link, including pairing events, so the
  peripheral GAP never acts on a link it doesn't own.

Everything else falls through unchanged, so on the light stack the firmware behaves
exactly like upstream.

**State lifetime.** `gap_central_init()` runs on GAP init. `gap_central_reset()` runs on
GAP stop. The module mutex is recursive and is held while callbacks run, which is what
makes the shutdown guarantee above work.
