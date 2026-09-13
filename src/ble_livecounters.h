#pragma once

// ============================================================================
// Live Counters characteristic — 6a40f003, Read + Notify, 14-byte payload
//
// UUID and layout from the app's allocation table and decodeLiveCounters();
// see docs/BLE_CONFIG_CONTRACT.md. The values themselves come from
// src/counters.h — this file is only the GATT surface.
//
// NOTIFY IS NOT OPTIONAL HERE
//
// BleService.ts subscribes with monitorCharacteristicForService() and its
// comment says "firmware notifies on every coin event". Two app features depend
// on the push rather than on polling:
//
//   * the post-sync live view ticks in real time without re-reading;
//   * the Diagnostics coin-path wizard takes a baseline, asks for a coin, and
//     watches for lifetimeAmount to move (APP_BLE_PLAN A3 — the sanctioned way
//     to confirm coin polarity without a serial console).
//
// So a coin must produce a notification promptly. ble_livecounters_service()
// does that, driven off counters_take_dirty() rather than a timer, so an idle
// board sends nothing and a busy one sends one frame per change.
//
// Adding f003 does NOT change the app's behaviour today: it decides 'full' vs
// 'configOnly' purely on f001 Device Info, so a board exposing f003 alone stays
// configOnly and the working Config flow is untouched. See src/ble_timesync.h
// for the full sequencing constraint.
// ============================================================================

#include <NimBLEDevice.h>

// Creates the characteristic on an existing service. Call from
// ble_config_init() before service->start().
void ble_livecounters_register(NimBLEService* service);

// Pushes a notification if the counters changed. Call from loop().
void ble_livecounters_service();
