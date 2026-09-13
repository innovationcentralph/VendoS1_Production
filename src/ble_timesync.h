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
// ⚠️ THE APP WILL NOT CALL THIS YET, AND THAT IS EXPECTED.
//
// The app decides a board's profile with a single test:
//
//     profile = uuids.includes(CHARACTERISTICS.deviceInfo) ? 'full' : 'configOnly'
//
// deviceInfo is f001. Until f001 exists the board stays 'configOnly', the app
// skips its whole sync sequence, and writeTimeSync is never called. Exposing
// f002 alone is therefore inert from the app's side — deliberately so: it is
// also SAFE, because nothing about today's working Config flow changes.
//
// The corollary is the sequencing constraint in docs/APP_BLE_PLAN.md: f001 must
// NOT ship on its own. The moment it appears the board reads as 'full' and the
// app runs writeTimeSync -> readDeviceInfo -> readLiveCounters -> readConfig ->
// pullSessionLogDelta -> readDiagnostics, and the first missing characteristic
// throws — taking the Config push that works today down with it. So f001, f002,
// f003, f004 and f006 land together or not at all.
//
// Until then this is exercised with nRF Connect, which talks to whatever GATT
// tree the board actually advertises and does not care about profiles.
// ============================================================================

#include <NimBLEDevice.h>

// Creates the Time Sync characteristic on an existing service. Call from
// ble_config_init() after the service exists and before service->start().
void ble_timesync_register(NimBLEService* service);
