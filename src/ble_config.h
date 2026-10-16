#pragma once

// =============================================================================
// BLE GATT config server — implements the Config characteristic defined in
// docs/BLE_CONFIG_CONTRACT.md (37-byte read/write blob covering AppConfig).
//
// Uses NimBLE-Arduino (see platformio.ini lib_deps) rather than the
// framework's built-in Bluedroid BLEDevice.h — Bluedroid cost ~825 KB flash
// for this one 37-byte characteristic (88.4% of the app partition); NimBLE is
// a fraction of that for the same GATT-server functionality. Gated behind
// ENABLE_BLE the same way the AT+ CLI is gated behind ENABLE_CLI: drop the
// build flag to remove the whole GATT server (and its flash footprint) from a
// production build until this is ready to ship.
// =============================================================================

// Call once from setup(), after config.cpp's storage is usable. Starts
// advertising immediately — see docs/BLE_CONFIG_CONTRACT.md for how to
// connect and exercise this with nRF Connect.
void ble_config_init();

// Can a phone get to this board right now — advertising, or already connected?
//
// Added for the OTA rollback guard (src/ota_guard.h), which confirms a freshly
// flashed image only once this is true: BLE is the way back in, so an image
// that cannot advertise must not be kept. Deliberately asks the stack rather
// than trusting a flag set at init — the question is whether the board is
// reachable now, not whether startAdvertising() once returned.
bool ble_config_is_reachable();
