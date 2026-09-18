#pragma once
#include <Arduino.h>
#include "pins.h"
#include "config.h"

// =============================================================================
// Hardware peripheral interface.
// All direct GPIO / tone operations go here — nothing else touches pins.
//
// Ported from the STM32 firmware src/periph.h. Differences from that file:
//
//   * watchdog_init()/watchdog_feed() are gone from here — the watchdog is now
//     external hardware with its own module. See src/wdt.h.
//   * The buzzer runs on LEDC rather than tone(), because ESP32 Arduino has no
//     tone() and BZ1 may be a passive sounder.
//   * The coin counter takes a polarity, since the S1 opto output swing is not
//     documented. See AppConfig::coin_active_high.
//   * periph_reset_safe() is new — ESP32 strapping pins need putting back to a
//     safe state before a software reset, which the STM32 did not.
// =============================================================================

// Call once at boot to configure all GPIO directions and safe initial states.
void periph_init();

// Put every strapping pin back to a state that is safe to reset through, then
// drop the relay. Call immediately before esp_restart().
//
// The STM32 needed no equivalent: its GPIOs all revert to inputs on reset and
// nothing was sampled at the reset instant. On the ESP32 three of our pins are
// strapping pins sampled at reset — IO12 (COIN_EN) high can force 1.8V flash
// timing and stop the board booting, IO2 (WDT_DONE) high picks an unintended
// boot mode, and IO15 (DBG_LED_B) gates the ROM boot log. External resistors
// cover a cold power-on; only firmware can cover a warm restart.
void periph_reset_safe();

// --- Relay ---
void relay_on();
void relay_off();

// --- Coin slot enable (IO12, active-HIGH) ---
// Call once at init (and after every session) to enable — HIGH = accepting coins.
// Call coin_slot_disable() only when a session starts with accumulation_enabled false.
void coin_slot_enable();   // IO12 HIGH — coins accepted
void coin_slot_disable();  // IO12 LOW  — coins inhibited
bool coin_slot_enabled();

// --- LEDs ---
// The STM32 had two debug LEDs (PA2/PA3) driven together; the S1 has one (D7 on
// IO15), so this drives that one. Same call sites, same meaning.
void leds_set(bool on);

// --- Buzzer ---
// Low double-beep  — "ready, go"  (called just before relay turns ON)
void buzzer_beep_session_start(const AppConfig* cfg);
// High triple-beep — "done done done" (called just after relay turns OFF)
void buzzer_beep_session_end(const AppConfig* cfg);
// Two short beeps to enter config mode (stub — not needed for happy path)
void buzzer_beep_enter_config();
// One long beep to exit config mode (stub)
void buzzer_beep_exit_config();
// Single low buzz — "denied" (start button pressed with insufficient credit)
void buzzer_beep_denied();

// --- Coin input ---
// Create mutex — call before any task that reads the counter starts.
void     coin_counter_init();
// FreeRTOS task body — polls PIN_COIN_IN with debounce, increments counter.
void     coin_counter_task_run(void* arg);
// Which level on PIN_COIN_IN counts as a pulse. Call once after config_load()
// and again after any config save. See AppConfig::coin_active_high for why this
// is configurable here but was hard-coded on the STM32.
void     coin_counter_set_polarity(bool active_high);
bool     coin_counter_polarity();
// Live level on PIN_COIN_IN, for AT+COIN? bench checks.
bool     coin_raw_level();

// Live, undebounced state of PIN_USER_BTN (START, J1 pin 1): true = pressed.
// Polarity is fixed, not configurable - the button has an external pull-up and
// shorts to ground, so pressed is LOW (see the INPUT note in pins.h).
//
// Undebounced ON PURPOSE. This is for reporting a level, not for detecting a
// press: use start_button_pressed() for that, which owns the one debounce state
// machine all three START meanings come from and must not be bypassed.
bool     user_btn_raw_pressed();
// Returns true if at least one credit has been counted since last consume/reset.
bool     coin_detected();
// Thread-safe read of current raw pulse count (diagnostic — billing runs off
// coin_get_value_cents() instead; see below).
uint32_t coin_get_count();
// Thread-safe reset to zero (both raw pulse count and money total).
void     coin_reset();

// ESP32-only, TEST MODE: while on, a debounced pulse still increments the raw
// count (so app_test_pulses() and the LCD keep working) but is NOT banked as
// credit and NOT recorded as earnings - it goes to counters_record_test_coin()
// instead. A test coin is the technician's, not revenue, and a banked one
// would be handed to the next real customer as free credit.
//
// Only the app task calls this, and ORDER MATTERS: turn it on before the slot
// can open in test mode, and off BEFORE coin_slot_enable() on exit, or a real
// customer's coin could land in the window and go unbilled.
void     coin_set_test_capture(bool on);

// --- Coin value (money) tracking — independent running total, in centavos ---
// Set the price (centavos) applied to each NEW coin pulse from here on. Call
// once after config_load() and again after any config-menu save, so a mid-run
// price edit does not retroactively reprice coins already banked.
void     coin_counter_set_price_per_credit(uint32_t price_cents);
// Thread-safe read of the accumulated coin value (centavos). This is its own
// running total incremented per-pulse at the price in effect when that pulse
// was detected — NOT raw pulses x current price computed at read time.
uint32_t coin_get_value_cents();
// Thread-safe decrement of the money total by `cents` (floors at 0). Call to
// spend a computed peso amount (e.g. whole credits billed) directly — the
// billing model is money-first, so this does NOT touch the raw pulse count.
// Any leftover cents simply remain banked for the next credit/session.
void     coin_consume_value_cents(uint32_t cents);

// --- Buttons ---
//
// START (PIN_USER_BTN) carries three distinct meanings on this board, because it
// absorbed the STM32 BTN3 page/cancel duties (see the gesture map in pins.h).
// All three are served by ONE shared state machine, so a single physical press
// can only ever satisfy one of them.

// Fires once on a confirmed press of PIN_USER_BTN (debounced 60 ms), and does
// NOT auto-repeat while held regardless of hold duration.
//
// The no-auto-repeat part is load-bearing, not incidental. It shares its
// debounce helper with btn_b1_pressed()/btn_b2_pressed(), which DO auto-repeat
// for config value scrubbing — and when that repeat leaked into the start
// button it made OP_PAUSE_RESUME chatter the relay ON/OFF every ~160 ms (then
// every ~40 ms once accelerated) for as long as anyone held the button. See the
// STM32 fix commit 8e0c203 and BUGS_HANDOVER.md item 1. The `repeat` gate in
// btn_simple_poll() exists solely to keep these two behaviours apart.
bool start_button_pressed();

// Fires on RELEASE, only if released before the long-press threshold.
// Config menu only: next page. Kept separate from start_button_pressed() so a
// press that turns out to be a long press never also counts as a page turn.
bool start_button_short_released();

// Fires once while still held, at ~1.5 s. Config menu only: cancel.
bool start_button_long_pressed();

// Discard any pending START one-shots. Call on entry to and exit from the config
// menu: the press that got you there must not also turn a page, and the press
// that saved or cancelled must not also start a session on the way out.
void start_button_flush();

// Returns true on a confirmed press of PIN_BTN1 (SW2), and again repeatedly
// while held — auto-repeat starts ~500 ms into the hold at a ~160 ms cadence,
// then accelerates to a ~40 ms cadence after ~2 s held, for fast config value
// scrubbing. Active-LOW.
//
// Suppressed while PIN_BTN2 is also down, because that is the combo gesture and
// not a value edit. Unlike the STM32, where the combo meant cancel and any
// scrubbing it caused was discarded, here it means SAVE — so an unsuppressed
// press would commit whatever the hold happened to change. See the note in
// periph.cpp for the specific way this went wrong on the boolean pages.
bool btn_b1_pressed();

// Returns true on a confirmed press of PIN_BTN2 (SW3), with the same
// hold-to-repeat/accelerate behavior as btn_b1_pressed(), and the same
// suppression while PIN_BTN1 is down. Active-LOW.
bool btn_b2_pressed();

// Returns true once when PIN_BTN1 and PIN_BTN2 have both been held down
// together for ~1.5 s. This is the S1 combo gesture, and it means two different
// things depending on where you are:
//   IDLE -> enter config mode   (STM32 used BTN3 long press)
//   MENU -> save and exit       (STM32 used BTN3 long press)
// Fires once per hold and will not re-fire until both buttons are released, so
// the press that enters the menu cannot immediately also save it.
bool btn_b1_b2_long_pressed();

// --- Sensor input (Sensor Stop) ---
// Raw read of PIN_SENSOR_IN resolved against active_high polarity — true if
// the pin currently reads its configured ACTIVE level. No debounce here; the
// caller (session_run) owns the trigger-timing policy.
bool sensor_read_active(bool active_high);

// --- Board role strap (JP10 / IO5) ---
// Reported by AT+STRAP? and the boot banner. Not acted on yet — the master/slave
// split is the next phase. See docs/PENDING.md.
bool strap_role_high();
