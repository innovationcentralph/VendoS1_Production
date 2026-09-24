#pragma once

// ============================================================================
// Device Info characteristic — 6a40f001, Read
//
//     schema_version  u8      must be 1; the app rejects anything else
//     modules_bitmap  u8      bit0 door, bit1 vibration, bit2 buzzer, bit3 gsm
//     device_id       u8 len + UTF-8 bytes
//     model           u8 len + UTF-8 bytes
//     fw_version      u8 len + UTF-8 bytes
//
// Layout from the app's decodeDeviceInfo() (mobile/src/ble/codec.ts). Note the
// strings are LENGTH-PREFIXED, not fixed-width — so device_id has no size
// ceiling to design around and no padding to agree on.
//
// ---------------------------------------------------------------------------
// THIS IS A CAPABILITY PROBE — its presence makes the board "full".
// ---------------------------------------------------------------------------
//
// BleService.resolveCapabilities() decides a board's whole profile on one test:
//
//     profile = uuids.includes(CHARACTERISTICS.deviceInfo) ? 'full' : 'configOnly'
//
// Always built (2026-09-23), so an S1 is always 'full'. The app runs the full
// chain on every connect:
//
//     writeTimeSync -> readDeviceInfo -> schema check -> readLiveCounters
//       -> readConfig -> pullSessionLogDelta -> readDiagnostics -> readWifiStatus
//
// and the first missing characteristic throws, so f002/f003/f004/f006 must stay
// registered alongside this one (readWifiStatus is caught by the app).
//
// ⚠️ App-side hazard, APP_BLE_PLAN A7: on a `full` connect the app asserts
// deviceInfo.deviceId === machine.deviceId. A machine claimed while the board
// was configOnly stored the BLE MAC there, and fails with "Connected to the
// wrong board" until it is re-claimed.
//
// See docs/APP_BLE_PLAN.md B1 and docs/BLE_CONFIG_CONTRACT.md.
// ============================================================================

#include <NimBLEDevice.h>

// Must equal SUPPORTED_SCHEMA_VERSION in mobile/src/ble/constants.ts. The app
// refuses to sync on any mismatch in either direction, so bumping this is a
// coordinated two-sided release, never a unilateral change.
#define BLE_DEVICEINFO_SCHEMA_VERSION 1

// modules_bitmap positions, from the app's ModuleBit enum.
#define BLE_MODULE_BIT_DOOR      0
#define BLE_MODULE_BIT_VIBRATION 1
#define BLE_MODULE_BIT_BUZZER    2
#define BLE_MODULE_BIT_GSM       3

// Creates the characteristic on an existing service. Call from
// ble_config_init() before service->start().
void ble_deviceinfo_register(NimBLEService* service);
