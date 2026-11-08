#pragma once

#include <Arduino.h>

// =============================================================================
// OTA rollback guard — "did the image we just booted actually work?"
//
// WHAT THE BOOTLOADER ALREADY DOES, AND WHY IT WAS DOING NOTHING
//
// The bootloader on this chip is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
// (verified 2026-10-15 by reading the bootloader off a real board: it is
// byte-identical to the framework's stock bootloader_dio_40m). That means an
// image written by OTA boots in state PENDING_VERIFY, and if the board resets
// before something marks it valid, the bootloader reverts to the previous slot.
//
// That safety net was inert here. Arduino calls esp_ota_mark_app_valid_cancel_
// rollback() from initArduino() — BEFORE setup() runs — unless the sketch
// overrides the weak verifyRollbackLater() to return true (esp32-hal-misc.c:225).
// This firmware did not, so every OTA image was declared good before a single
// line of it had executed. An image that boots but cannot advertise over BLE
// would have been kept, and the only way back is a serial cable: a site visit
// per board, for the exact failure OTA exists to avoid.
//
// WHAT THIS DOES INSTEAD
//
// verifyRollbackLater() returns true, so the image stays PENDING_VERIFY, and
// this module confirms it only after the board proves it is reachable:
//
//   * the BLE stack is up and either advertising or connected — this is the one
//     that matters, because it IS the way back in. A board that vends perfectly
//     but cannot be reached can never be updated again over the air.
//   * the app task has checked in recently (watchdog_liveness_stale_ms) — the
//     board is vending, not just advertising.
//   * both have held for OTA_GUARD_SELFTEST_MS.
//
// THE WINDOW IS SHORT ON PURPOSE
//
// While PENDING_VERIFY, any reset — including a power cut — rolls the board
// back. On a coin machine mains flicker is routine, so a long self-test would
// quietly revert good updates and present as "the update didn't take". 15 s is
// long enough for BLE to come up and the app task to tick, short enough that
// landing a power cut inside it is unlucky rather than expected.
//
// And if the criteria are NOT met, this rolls back deliberately
// (OTA_GUARD_TIMEOUT_MS) rather than waiting for a power cut to do it. A board
// that reverts within a minute is a failed update; one that sits broken until
// the next outage is a mystery.
//
// NOTHING HERE BLOCKS. Per src/wdt.h a blocking call is a reboot, not a stall,
// and a reboot in this state is a rollback.
// =============================================================================

// How long the health criteria must hold before the image is confirmed.
#define OTA_GUARD_SELFTEST_MS   15000

// How long to wait for them before giving up and rolling back on purpose.
#define OTA_GUARD_TIMEOUT_MS    60000

// Call once in setup(), after BLE and the app task exist but before loop().
// Reads whether this boot is an unconfirmed OTA image and says so on Serial.
void ota_guard_init();

// Call from loop(). Cheap (two millis() compares) and a no-op on an ordinary
// boot — only a freshly OTA'd image is ever pending.
void ota_guard_service();

// True while this boot is still unconfirmed. The app shows it so an operator
// is not told an update succeeded while it can still revert.
bool ota_guard_pending();

// AT+OTA? — what state this boot is in, which slot is running, and what the
// board would fall back to.
void ota_guard_print_status(Stream& out);
