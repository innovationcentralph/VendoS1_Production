#pragma once

// ============================================================================
// Session Log characteristic — 6a40f004, Read + Write, stateful pagination
//
// UUID and wire format from the app's allocation table, mobile/src/ble/codec.ts
// and BleService.pullSessionLogDelta(); see docs/BLE_CONFIG_CONTRACT.md. The
// events themselves come from src/eventlog.h — this file is only the GATT
// surface and the cursor bookkeeping.
//
// THE PROTOCOL IS WRITE-THEN-READ ON ONE CHARACTERISTIC
//
//   app writes  after_seq:u32 LE          (4 bytes)
//   app reads   count:u8 | has_more:u8 | count x 14-byte events
//   app repeats with after_seq = the LAST SEQ IT RECEIVED, until has_more is 0
//
// `after_seq` is EXCLUSIVE on every page, not just the first — the app's own
// comment says so, and it advances its cursor to the last seq received rather
// than that plus one. eventlog_build_page() filters `seq > cursor` every time.
//
// THE CURSOR IS PER CONNECTION
//
// The ESP32 permits three simultaneous connections. One global cursor would
// mean two phones pulling at once silently corrupt each other's pagination —
// each would advance the other's position and both would file incomplete
// history. So the cursor is keyed on the connection handle NimBLE hands the
// callback, and released when that connection drops (ble_sessionlog_on_disconnect,
// wired from the server callbacks in ble_config.cpp).
//
// ONE REMAINING SHARED-STATE CAVEAT, DOCUMENTED RATHER THAN HIDDEN
//
// The cursor is per connection but the GATT attribute VALUE is not — there is
// one buffer per characteristic. NimBLE skips the read callback on long-read
// continuations (NimBLECharacteristic.cpp checks om_pkthdr_len), so a page
// larger than the MTU is delivered from a stable buffer; but if two clients
// were both doing long reads at the same instant, one could collect a segment
// of the other's page. A 142-byte page needs a long read only at the default
// 23-byte MTU: the app negotiates 247 and never gets there. Not worth a second
// characteristic to fix, worth knowing before someone reports it.
// ============================================================================

#include <NimBLEDevice.h>

// Creates the characteristic on an existing service. Call from
// ble_config_init() before service->start(), after eventlog_init().
void ble_sessionlog_register(NimBLEService* service);

// Frees the cursor slot a connection was using. Call from the server's
// onDisconnect so a recycled connection handle cannot inherit a stale cursor.
void ble_sessionlog_on_disconnect(uint16_t conn_handle);
