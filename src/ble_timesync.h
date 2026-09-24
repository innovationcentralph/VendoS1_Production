#pragma once

// ============================================================================
// Time Sync characteristic — 6a40f002, Write, payload epoch_utc:u32 (LE)
//
// UUID and payload come from the app team's allocation table, recorded in
// docs/BLE_CONFIG_CONTRACT.md. Do not invent either.
//
// WHY THIS IS A SEPARATE FILE
//
// ble_config.cpp owns the GATT server and the Config characteristic, and is
// hardware-verified. Five more characteristics are coming (f001 Device Info,
// f003 Live Counters, f004 Session Log, f006 Diagnostics, f007 Command, plus the
// OTA/WiFi set), so each gets its own translation unit and registers itself into
// the one shared service rather than growing ble_config.cpp into a grab bag.
//
// The app decides a board's profile with a single test:
//
//     profile = uuids.includes(CHARACTERISTICS.deviceInfo) ? 'full' : 'configOnly'
//
// deviceInfo is f001, which is always built (2026-09-23), so an S1 is always
// 'full' and the app runs writeTimeSync -> readDeviceInfo -> readLiveCounters ->
// readConfig -> pullSessionLogDelta -> readDiagnostics on every connect. The
// first missing characteristic throws and fails the whole connect, so f001,
// f002, f003, f004 and f006 must all stay registered together.
// ============================================================================

#include <NimBLEDevice.h>

// Creates the Time Sync characteristic on an existing service. Call from
// ble_config_init() after the service exists and before service->start().
void ble_timesync_register(NimBLEService* service);
