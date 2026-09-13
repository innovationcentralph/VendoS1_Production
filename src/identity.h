#pragma once

// ============================================================================
// Board identity — the factory serial that the app uses as `device_id`
//
// ESP32/S1 ONLY. There is no STM32 counterpart to diff against; this is new
// capability, not a port. See docs/PORTING_FROM_STM32.md and
// docs/APP_BLE_PLAN.md items D2 / D8 / X6.
//
// WHY THIS IS NOT IN AppConfig
//
// The obvious place for a serial is a field in AppConfig, and it would be
// wrong, for three independent reasons:
//
//   1. config_load() falls back to config_defaults() on a magic mismatch or a
//      short blob. A serial living there would be SILENTLY REASSIGNED as a
//      side effect of that guard working correctly.
//   2. CLAUDE.md requires bumping APP_CONFIG_MAGIC on any AppConfig layout
//      change. Every future config field would therefore erase every board's
//      serial in the field.
//   3. A factory reset (AT+EEPROM_RESET) is meant to clear *settings*. Losing
//      identity is not a setting.
//
// So identity lives in its own NVS namespace, and NOTHING in config.cpp reads
// or writes it. The standing rule, worth keeping in mind if anyone later adds
// the event log's sequence counter (docs/APP_BLE_PLAN.md B3): **identity and
// monotonic counters are not configuration.** Config is resettable by design;
// these must not be.
//
// A pleasant side effect: no AppConfig change means no magic bump and no
// change to the hardware-verified 37-byte BLE Config frame.
//
// WHAT WIPES THE SERIAL
//
// `pio run -t upload` preserves NVS, so reflashing is safe. These do not:
//   * esptool erase_flash          <- the one that catches people out
//   * an NVS partition resize, or any partition-table change
// A board wiped this way reports unprovisioned and looks perfectly healthy,
// which is why AT+SERIAL? and the boot banner both say so out loud.
// ============================================================================

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Serial format: "VLABS-S1-NNNNN" — fixed prefix + exactly 5 digits, 14 chars.
//
// The prefix is abbreviated on purpose. It is identical on every board, so it
// carries no discriminating information — its only job is making a serial
// recognizable to a human on a sticker or in a support call, and 5 characters
// does that as well as spelling the brand out would. Note "PH" is part of the
// company name rather than a region marker, so nothing here varies by market.
//
// TWO CONSEQUENCES WORTH KNOWING, both permanent once boards are provisioned:
//
//   * 5 digits caps the fleet at 99,999 units. Raising IDENTITY_SERIAL_DIGITS
//     later works, but 5- and 6-digit serials would then coexist in the field
//     and sort/display inconsistently. Decide the ceiling before the first
//     production run, not after.
//   * 14 chars leaves the BLE Device Info wire field (docs/APP_BLE_PLAN.md B1,
//     24 bytes NUL-padded) 9 bytes of slack. That slack is the point: a format
//     change that still fits is a one-line edit HERE, invisible to the app. One
//     that does not fit shifts every later field's offset, which means a
//     schema_version bump and an app that parses both layouts forever.
//
// Everything below is derived from PREFIX and DIGITS — the static_asserts in
// identity.cpp fail the build if LEN or BUF stops agreeing with them, and the
// CLI's help and error strings are built from the PATTERN/EXAMPLE macros rather
// than spelling out a digit count that would drift.
// ---------------------------------------------------------------------------
#define IDENTITY_SERIAL_PREFIX "VLABS-S1-"
#define IDENTITY_SERIAL_DIGITS 5
#define IDENTITY_SERIAL_LEN    14   // strlen(PREFIX) + DIGITS
#define IDENTITY_SERIAL_BUF    15   // + NUL

// Stringify, so a digit count is never written out by hand.
#define IDENTITY_STR_(x) #x
#define IDENTITY_STR(x)  IDENTITY_STR_(x)
#define IDENTITY_SERIAL_DIGITS_STR IDENTITY_STR(IDENTITY_SERIAL_DIGITS)

// Human-facing forms, for CLI help and error text.
#define IDENTITY_SERIAL_PATTERN IDENTITY_SERIAL_PREFIX "NNNNN"
#define IDENTITY_SERIAL_EXAMPLE IDENTITY_SERIAL_PREFIX "00123"

// NVS namespace + key. Namespace is capped at 15 chars by NVS. Deliberately
// distinct from APP_CONFIG_NS ("vendo") so no config path can touch it.
#define IDENTITY_NS  "vendoid"
#define IDENTITY_KEY "serial"

// Erase token: the last 3 bytes of this board's MAC, as 6 uppercase hex
// characters. Not a secret — the point is that erasing an identity should
// require being able to read the board you are holding, so a broadcast or
// mistyped command cannot orphan the wrong unit.
#define IDENTITY_TOKEN_LEN 6
#define IDENTITY_TOKEN_BUF 7

enum IdentitySetResult {
    IDENTITY_SET_OK = 0,
    IDENTITY_SET_ALREADY,   // write-once: a valid serial is already stored
    IDENTITY_SET_BAD_FORMAT,
    IDENTITY_SET_NVS_FAIL,
};

// True if a well-formed serial is stored. A stored value that fails the format
// check reads as INVALID rather than being reported as an identity — a garbled
// NVS entry should present as "unprovisioned", not as a plausible-looking board.
bool identity_serial_valid();

// Copies the stored serial into out. Returns false (and writes an empty
// string) if none is stored or it is malformed.
bool identity_serial_get(char out[IDENTITY_SERIAL_BUF]);

// Validates s against IDENTITY_SERIAL_PREFIX + 6 digits. Prefix comparison is
// case-insensitive; identity_serial_set() stores the canonical spelling, so two
// boards can never differ only by case.
bool identity_serial_format_ok(const char* s);

// Write-once. Fails with IDENTITY_SET_ALREADY if a valid serial is already
// stored — deliberately, so nobody with a cable at J7 can retype one board's
// identity onto another and merge their earnings into one backend row.
// Use identity_serial_erase() for the RMA/refurb path.
IdentitySetResult identity_serial_set(const char* s);

// Clears the stored serial. Requires the token from identity_erase_token().
bool identity_serial_erase(const char* token);

// This board's base MAC (factory-immutable, needs no provisioning). The BLE
// Device Info characteristic must report this ALONGSIDE the serial: it is the
// only key the backend can use to match an already-claimed board to its
// existing row, because those rows currently hold a MAC in `Machine.deviceId`.
// Without it, every board claimed before Device Info shipped becomes an orphan
// with its earnings history stranded. See docs/APP_BLE_PLAN.md D2 section 3.
void identity_mac(uint8_t out[6]);

// The erase token for this board, as 6 uppercase hex chars + NUL.
void identity_erase_token(char out[IDENTITY_TOKEN_BUF]);

// One-line "serial = ... / UNPROVISIONED" report, used by the boot banner and
// AT+SERIAL?.
void identity_print(Stream& out);
