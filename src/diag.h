#pragma once

// ============================================================================
// Diagnostics — the fault mailbox behind BLE characteristic 6a40f006
//
// ESP32/S1 ONLY. No STM32 counterpart; intended divergence, recorded in
// docs/PORTING_FROM_STM32.md §2.5a.
//
// ---------------------------------------------------------------------------
// WHY A MAILBOX AND NOT A STATUS SNAPSHOT
// ---------------------------------------------------------------------------
//
// The error list is **cumulative until acknowledged**, not a picture of what is
// wrong right now. The app reads it on connect, uploads each code to the backend
// as a `board_error` event, and only then sends the Command op `clear_errors`
// to empty it. Their own comment: *"the board's error list is cumulative until
// something acks it… without this the same faults re-upload on every connect."*
//
// So a fault that happened at 03:00 and cleared itself must still be sitting
// here when an operator connects at noon. That retention IS the diagnostic
// value — a vending board has no fault display and nobody reads a serial
// console in the field, so this list is the only channel to a human.
//
// ---------------------------------------------------------------------------
// RETENTION: RAM ONLY, AND WHY THAT IS ACCEPTABLE HERE
// ---------------------------------------------------------------------------
//
// Raised codes live in RAM and are lost on reboot. That sounds like it defeats
// the point, and mostly does not, because **every condition we can currently
// detect re-asserts itself**:
//
//   0x03 RTC unset      — re-derived from the chip at every boot
//   0x05 Watchdog reset — read from the reset-reason register at every boot
//   0x04 NVS write fail — a failing flash fails again on the next write
//   0x01 Coin line      — a stuck line is still stuck after a reboot
//
// What is genuinely lost is a *transient historical* fault that has since
// cleared. Persisting to NVS would fix that, and is deliberately not done:
// 0x04 exists to report that NVS writes are failing, so writing it to NVS is
// circular. If a later code needs true durability, give it its own namespace
// (the standing rule from src/identity.h) rather than folding it into config.
//
// ---------------------------------------------------------------------------
// WHAT IS DELIBERATELY NOT REPORTED
// ---------------------------------------------------------------------------
//
// **0x02 Relay drive fault is never raised.** This board has no relay current
// sense, so firmware cannot observe the condition. Reporting it on inference
// would be worse than silence — an operator would chase a fault nobody can
// confirm. It stays defined so the code is not reused for something else.
//
// **Panic and brownout resets are not reported at all.** They matter as much as
// a watchdog reset, but the app's table has no code for them and minting one
// from this side risks colliding with a future allocation — the same rule that
// governs UUIDs. Raise it with the app team; do not invent 0x06.
// ============================================================================

#include <Arduino.h>
#include <stdint.h>

// Error codes — from the app's DiagnosticsErrorCode table
// (mobile/src/ble/constants.ts). Do not add to these without an allocation.
#define DIAG_ERR_COIN_LINE   0x01   // coin input stuck asserted
#define DIAG_ERR_RELAY_DRIVE 0x02   // NEVER RAISED — no relay sense on this board
#define DIAG_ERR_RTC_UNSET   0x03   // clock never set, or backup cell dead
#define DIAG_ERR_NVS_WRITE   0x04   // a persistent write failed
#define DIAG_ERR_WDT_RESET   0x05   // watchdog reset since the last sync

// sensors_bitmap positions — from the app's SensorBit enum.
#define DIAG_SENSOR_BIT_DOOR      0
#define DIAG_SENSOR_BIT_VIBRATION 1   // no distinct input on the S1 — always 0
#define DIAG_SENSOR_BIT_COIN      2
#define DIAG_SENSOR_BIT_RTC       3

// ---------------------------------------------------------------------------
// Bit 4 = the user button (START, J1 pin 1). NOT ALLOCATED BY THE APP TEAM.
// ---------------------------------------------------------------------------
// Built and OFF by default, the same way ENABLE_BLE_DEVICE_INFO is, and for the
// same reason: SensorBit is the APP's enum, and a bit we claim that they later
// assign to something else does not error - it renders as whatever they made it
// mean. A button press showing up as a door being opened is exactly the silent
// misparse the allocation rule exists to prevent.
//
// Ask for it as `SensorBit.button = 4`: one bit, backward compatible in both
// directions (an old app build ignores an unknown bit; a new one reading a board
// that never sets it just sees 0). That is a far cheaper ask than a counter
// field, which cannot be appended to this frame at all - the fault list is
// variable-length and terminal, so anything added after the header shifts it.
//
// When they say yes: move -DENABLE_DIAG_BUTTON_BIT into [env:esp32dev] and
// delete this comment. See APP_BLE_PLAN A14.
#ifdef ENABLE_DIAG_BUTTON_BIT
#define DIAG_SENSOR_BIT_BUTTON    4
#endif

// Wire frame: uptime u32, boot_ts u32, boot_ts_unverified u8, sensors u8,
// errors_count u8, then that many code bytes. Variable length.
#define DIAG_WIRE_HEADER_LEN 11
#define DIAG_ERR_MAX         5
#define DIAG_WIRE_MAX        (DIAG_WIRE_HEADER_LEN + DIAG_ERR_MAX)

// Latches the boot-time conditions: reset reason (0x05) and clock validity
// (0x03). Call after rtc_init() and after wdt_begin(), both of which it reads.
void diag_init();

// Raise a fault. Idempotent — raising the same code twice is one entry, so a
// repeatedly-failing write does not flood the list.
void diag_raise(uint8_t code);

// Empty the list. This is the Command characteristic's `clear_errors` op
// (0x02); until f007 exists, `AT+DIAG_CLEAR` is the only way to reach it.
void diag_clear_all();

// Live input states, assembled per read — see DIAG_SENSOR_BIT_*.
uint8_t diag_sensors_bitmap();

// Serialises the frame. Returns the byte count written (11..16).
size_t diag_serialize(uint8_t* out, size_t cap);

// True if anything changed since the last call, so the BLE layer can notify
// only when there is news. Clears the flag.
bool diag_take_dirty();

// Re-evaluates conditions that can change while running (currently the clock).
// Call from loop().
void diag_service();

void diag_print(Stream& out);
