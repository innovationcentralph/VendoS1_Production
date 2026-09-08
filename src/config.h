#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <Arduino.h>   // for Stream

// =============================================================================
// App Configuration — persisted to NVS (ESP32 flash, via the Preferences API).
//
// The STM32 stored this with EEPROM.get/put() into its flash-emulated EEPROM at
// byte address 0. The ESP32 has no EEPROM emulation worth using here, so the
// whole struct is written as one NVS blob instead. Consequences, all of them
// improvements:
//
//   * No page-erase-per-byte problem. On STM32F1 every emulated-EEPROM byte
//     write erased and reprogrammed a 1 KB page, which is why the sibling
//     slave firmware had to hand-roll a buffered write path. NVS batches and
//     wear-levels for us, so config_save() is one call and one commit.
//   * The magic check still earns its keep — NVS returning a short/absent blob,
//     or a struct layout change, both land as "stale" and fall back to defaults
//     exactly as the STM32 did.
//
// Bump APP_CONFIG_MAGIC whenever the struct layout changes so an old stored
// config is rejected and defaults are written instead.
// =============================================================================

// Diverged from the STM32 firmware's 0xBEEFCB07 because this struct has one
// extra trailing field (coin_active_high — see below). Keeping the STM32 value
// would let an S1 board accept a byte pattern it cannot correctly parse. The
// low byte tracks the STM32 revision this was ported from; 51 reads as "S1".
#define APP_CONFIG_MAGIC  0xBEEF5107UL

// NVS namespace + key. Namespace is capped at 15 chars by NVS.
#define APP_CONFIG_NS     "vendo"
#define APP_CONFIG_KEY    "appcfg"

// Display module selection — stored in AppConfig::display_type
typedef enum : uint8_t {
    DISPLAY_NONE      = 0,  // no display attached
    DISPLAY_LCD_16X2  = 1,  // I2C 16x2 LCD (PCF8574 backpack, addr 0x27)
    DISPLAY_7SEG_4DIG = 2,  // TM1637 4-digit 7-segment (stub, as on the STM32)
} DisplayType;

// Button/relay/timer operation mode — stored in AppConfig::operation_mode.
// OP_PRESS_TO_START and OP_PAUSE_RESUME are implemented in session_run();
// the remaining values are reserved so the FSM can dispatch on this field
// as each mode is added (see NUM_OPERATION_MODES_IMPLEMENTED below).
typedef enum : uint8_t {
    OP_PRESS_TO_START = 0,  // press once, relay ON for the full timer, button ignored while running
    OP_PAUSE_RESUME   = 1,  // button toggles relay ON/OFF, timer keeps counting down regardless
    OP_ON_OFF_TOGGLE  = 2,  // button toggles relay ON/OFF, timer pauses while relay is OFF
    OP_AUTO_START     = 3,  // no button — relay/timer start as soon as credit is satisfied
    OP_SENSOR_STOP    = 4,  // reserved value only — Sensor Stop is NOT a selectable start
                             // method. It is implemented as the composable AppConfig::
                             // sensor_stop_enabled flag instead, since the spec allows it
                             // to pair with EITHER Press-to-Start OR Auto Start.
} OperationMode;

// Number of OperationMode values actually implemented in session_run() today.
// Bump this (and kImplementedOpModes below) as each mode lands.
#define NUM_OPERATION_MODES_IMPLEMENTED  3   // OP_PRESS_TO_START, OP_PAUSE_RESUME, OP_AUTO_START

// Ordered list of the OperationMode values actually implemented today. Modes
// are numbered to match the spec Mode 1..5, so implemented modes are NOT
// necessarily contiguous (Auto Start=3 shipped before ON/OFF Toggle=2) — the
// LCD menu and CLI must cycle/validate against this list, never against the
// raw range [0, NUM_OPERATION_MODES_IMPLEMENTED), or they can land on an
// unimplemented value that silently falls through to default behavior.
extern const uint8_t kImplementedOpModes[NUM_OPERATION_MODES_IMPLEMENTED];

struct AppConfig {
    uint32_t magic;

    // --- Core vend behaviour ---
    uint32_t relay_on_ms;           // relay ON duration per satisfied credit
                                     // (see coins_required), not per raw pulse (default 5000)
    uint32_t coins_required;        // the PESO PRICE of one credit (coins_required * 100
                                     // = centavos) — NOT a raw pulse count. Money inserted
                                     // (coin_get_value_cents(), see price_per_credit_cents
                                     // below) is floored to a whole multiple of this price
                                     // before billing; the remainder is carried over (kept,
                                     // not consumed) for the next credit, whether that is
                                     // later this session (see accumulation_enabled) or the
                                     // next one (default 1 = P1.00/credit)
    uint8_t  operation_mode;        // OperationMode enum (default OP_PRESS_TO_START)

    // --- Per Credit options (OP_PRESS_TO_START or OP_PAUSE_RESUME only —
    //     LCD only shows these in those two modes) ---
    bool     press_to_start_per_credit; // OP_PRESS_TO_START or OP_PAUSE_RESUME only:
                                     // dispense one credit block per button press
                                     // instead of all billed blocks in one continuous
                                     // session — e.g. 4 credits bought -> press once,
                                     // run 1 block, stop; press again for the next,
                                     // etc. (default false = all-at-once behavior)
    uint16_t inactivity_timeout_s;   // single shared timeout (seconds) covering two
                                     // safety nets: 1) auto-starts the initial credit in
                                     // OP_PRESS_TO_START/OP_PAUSE_RESUME if ready but the
                                     // button is not pressed; 2) in press_to_start_per_credit
                                     // mode, auto-advances to the next credit if the button
                                     // is not pressed between them. 0 = disabled, wait
                                     // indefinitely (default)

    // --- Coin Accumulation (composable with any operation_mode above) ---
    bool     accumulation_enabled;   // when true, coins inserted while a session is
                                     // already running are accepted and added to the
                                     // remaining time (one relay_on_ms chunk per whole
                                     // block) instead of just sitting uncounted until
                                     // the next session (default false = disabled).
                                     // Also controls whether the coin slot stays live
                                     // during an active session: ON keeps it enabled
                                     // (needed to accept the mid-session coins above),
                                     // OFF disables it for the whole session — there is
                                     // no separate coin-slot toggle anymore (see
                                     // session_run()).

    // --- Sensor Stop (composable with any operation_mode above) ---
    bool     sensor_stop_enabled;    // when true, an active session also ends early if
                                     // the sensor triggers (default false = disabled)
    bool     sensor_active_high;     // true = triggered when PIN_SENSOR_IN reads HIGH,
                                     // false = triggered on LOW (default false — matches
                                     // common active-LOW switch/module wiring)
    uint16_t sensor_wait_ms;         // continuous-active time required before the sensor
                                     // counts as triggered — 0 = immediate (default 50)

    // --- Feature flags ---
    bool     gsm_reporting_enabled;  // send GSM report after each session (default false)

    // --- Display module ---
    uint8_t  display_type;          // DisplayType enum (default DISPLAY_LCD_16X2)

    // --- Start beep (low pitch, fewer counts — "ready") ---
    uint16_t beep_start_freq_hz;    // default 1000
    uint8_t  beep_start_count;      // default 2
    uint16_t beep_start_on_ms;      // default 150

    // --- End beep (high pitch, more counts — "done") ---
    uint16_t beep_end_freq_hz;      // default 2500
    uint8_t  beep_end_count;        // default 3
    uint16_t beep_end_on_ms;        // default 70

    // --- Gap between consecutive beeps ---
    uint16_t beep_off_ms;           // default 80

    // --- LCD money display ---
    uint32_t price_per_credit_cents; // peso value of ONE raw pulse from whatever
                                     // acceptor is wired in (coin acceptor: 1 pulse
                                     // per peso; bill acceptor: pulses per peso
                                     // configurable), in centavos. Feeds ONLY the
                                     // running "COIN:X.XX" money total (coin_get_
                                     // value_cents() = raw pulses * this rate) —
                                     // it does not affect the idle screen price
                                     // prompt or billing math, which use
                                     // coins_required directly (default 1000 = P10.00)

    // -------------------------------------------------------------------------
    // ESP32-only field — no STM32 counterpart. Appended last, and the reason
    // APP_CONFIG_MAGIC diverges from the STM32 value.
    // -------------------------------------------------------------------------
    bool     coin_active_high;      // which level on PIN_COIN_IN counts as a coin pulse.
                                     // The STM32 hard-coded "idle LOW, pulse HIGH" because
                                     // its acceptor wiring was known. On the S1 the pulse
                                     // passes through the PC817 (U4) opto and the schematic
                                     // does not say which way its output swings — the
                                     // bring-up harness had to make the counting edge
                                     // switchable for the same reason. This is money, so it
                                     // is a settable field rather than a guess baked into
                                     // the build: false (default) = idle HIGH, pulse LOW,
                                     // which is the usual opto-output-with-pull-up shape.
                                     // Confirm on the bench with AT+COIN? before shipping a
                                     // board (see docs/PENDING.md item 3).
};

// Load config from NVS into *cfg.
// Returns false if no valid config found; *cfg is filled with factory defaults
// (and those defaults are persisted, same as the STM32 did).
bool config_load(AppConfig* cfg);

// Read the stored config without logging and without writing anything back.
// Returns false (and leaves *cfg untouched) if no valid blob is stored.
//
// This is what the CLI setters use. config_load() is the boot path and has two
// side effects they must not trigger: it prints the whole config, which would
// bury the response to an AT+ command, and it persists defaults on a miss, which
// would turn a read-only query into a flash write.
bool config_load_quiet(AppConfig* cfg);

// Persist *cfg to NVS.
void config_save(const AppConfig* cfg);

// Fill *cfg with factory defaults.
void config_defaults(AppConfig* cfg);

// Print all fields of *cfg in human-readable form.
void config_print(const AppConfig* cfg, Stream& out);

// Read the stored blob and print:
//   1. Raw hex bytes (so you see exactly what is stored)
//   2. Re-parsed field values + magic validity check
// Call from setup() or debug task — does NOT modify RAM config.
// (Named for the STM32 original so AT+EEPROM? keeps meaning the same thing.)
void config_dump_eeprom(Stream& out);
