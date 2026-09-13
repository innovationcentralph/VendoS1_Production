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
// ⚠️ THIS IS A CAPABILITY PROBE. SHIPPING IT ALONE BREAKS THE APP.
// ---------------------------------------------------------------------------
//
// BleService.resolveCapabilities() decides a board's whole profile on one test:
//
//     profile = uuids.includes(CHARACTERISTICS.deviceInfo) ? 'full' : 'configOnly'
//
// Today an S1 is 'configOnly'. The app skips its entire sync sequence and goes
// straight to Config — which is exactly why Config works on a real board.
//
// The moment f001 appears the same board reads as 'full', and BleConnectionContext
// runs the full chain instead:
//
//     writeTimeSync -> readDeviceInfo -> schema check -> readLiveCounters
//       -> readConfig -> pullSessionLogDelta -> readDiagnostics -> readWifiStatus
//
// The first missing characteristic throws and the connect FAILS — taking the
// Config push that works today down with it. So exposing Device Info on its own
// is not an increment, it is a regression.
//
// f002 Time Sync and f003 Live Counters are already built. The remaining
// prerequisites are **f004 Session Log** and **f006 Diagnostics**. Only when
// those exist should this be switched on. (readWifiStatus is caught by the app,
// so the WiFi set is not a blocker.)
//
// That is why this file is behind ENABLE_BLE_DEVICE_INFO, and why the flag is
// OFF by default in platformio.ini. The hazard is a build-flag decision rather
// than a comment somebody has to remember at flash time. Enable it for
// nRF Connect testing — nRF talks to whatever GATT tree exists and has no
// notion of profiles — and leave it off for any board the app will touch.
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
// ble_config_init() before service->start(). No-op unless
// ENABLE_BLE_DEVICE_INFO is defined.
void ble_deviceinfo_register(NimBLEService* service);
