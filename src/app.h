#pragma once

#include <stdint.h>   // app_test_pulses() returns uint32_t; this header had no
                      // fixed-width types before test mode was added.

// =============================================================================
// Main Application State Machine
// Ported verbatim from the STM32 firmware src/app.h.
// =============================================================================
typedef enum {
    APP_STATE_INIT,           // Power On -> Read Config from NVS -> Init Hardware
    APP_STATE_IDLE,           // Wait for Coin | check config-mode gesture
    APP_STATE_CONFIG_MODE,    // Config Mode Enter (BTN1+BTN2 long press triggered)
    APP_STATE_COIN_VALIDATE,  // Coin Inserted -> Amount Valid?
    APP_STATE_WAIT_START,     // Wait for Start Button press
    APP_STATE_SESSION_ACTIVE, // Session Start -> Timer Mode (session.cpp)
    APP_STATE_WAIT_NEXT_CREDIT, // (OP_PRESS_TO_START or OP_PAUSE_RESUME) +
                                // press_to_start_per_credit only: one credit block
                                // just finished dispensing and more remain — wait
                                // for another button press (or the shared
                                // inactivity_timeout_s) before dispensing the next
    APP_STATE_SESSION_END,    // Relay Off -> Log -> optional GSM -> back to IDLE
    APP_STATE_TEST_MODE,      // ESP32-only: bench/diagnostic hold. No vend
                              // operation at all — see app_request_test_mode().
                              // Appended deliberately: the STM32 has no such
                              // state, and inserting it earlier would renumber
                              // states the two firmwares share.
} AppState;

// FreeRTOS task entry point — create this task in main.cpp setup().
void app_task_run(void* arg);

// ESP32/S1 ONLY — no STM32 counterpart, see docs/PORTING_FROM_STM32.md 2.5a.
//
// True when the board is not in the middle of anything a customer paid for or
// an operator is editing: IDLE only. Safe from any task: it reads a single
// aligned word published by the app task once per iteration of its state
// machine.
bool app_state_is_idle();

// True in IDLE *or* TEST_MODE — "is it safe to drive a physical output right
// now?", which is the question the BLE Command characteristic (6a40f007)
// actually needs answered before a relay or buzzer test. A relay click mid-vend
// is a customer complaint and a test beep reads as a fault, so a session or the
// config menu must refuse; TEST_MODE must NOT, because running those tests is
// the entire reason that state exists. Use this, not app_state_is_idle(), for
// any new gate on a test operation.
bool app_state_allows_physical_op();

// True while the board is in TEST_MODE. Status only - for a gate, use
// app_test_mode_engaged() instead.
bool app_test_mode_active();

// True while the board is in TEST_MODE *or* has an accepted entry request it
// has not acted on yet.
//
// WHY BOTH EXIST: the app sends "enter test mode" then "enable the coin slot"
// back to back, loop() drains one command per pass, and the app task only
// publishes TEST_MODE on its next 20 ms iteration. So the coin-slot op
// routinely arrives while the board is still, strictly speaking, IDLE. Gating
// it on app_test_mode_active() would refuse the app its own natural sequence
// perhaps half the time - a flaky failure far worse than a clear one. The
// request itself is latched either way: the app task parks the slot on entry
// and applies any pending request in the same iteration, so the ordering comes
// out right no matter which arrives first.
bool app_test_mode_engaged();

// Called by the app task itself, once per state-machine iteration. Not for
// anyone else to call.
void app_publish_state(AppState st);

// ESP32-only: ask the app task to blink PIN_USER_LED (the J1 illuminated
// button lamp) as a bench test. No STM32 counterpart.
//
// WHY THIS IS NOT JUST A leds_set()-STYLE CALL FROM loop():
// PIN_USER_LED is not a free pin. The app task re-asserts it every 20 ms in
// APP_STATE_IDLE ("ready — press the button"), so a blink driven from any other
// task is overwritten within 20 ms and the technician sees nothing. The pin's
// owner has to do it, which is what this request is for. The app task services
// it in IDLE and in TEST_MODE (the app runs its diagnostic checklist inside test
// mode, so IDLE-only meant the op was accepted and never run) and clears it;
// nothing is queued, so a request that arrives as a session starts is simply
// lost rather than replayed later — matching the "dropped, not deferred" rule
// the BLE Command ops follow (src/ble_command.h).
void app_request_user_led_test();

// =============================================================================
// TEST MODE — ESP32-only, no STM32 counterpart
// =============================================================================
//
// A bench/diagnostic hold. While in it the board performs NO vend operation:
// coins start no session, the START button does nothing, Auto Start does not
// fire, the inactivity timeout does not run, and the relay stays off. It sits
// waiting for BLE Command ops (and the CLI).
//
// ⚠️ A BOARD IN TEST MODE IS OUT OF SERVICE. THREE WAYS OUT, ON PURPOSE.
//
// The obvious failure is a technician whose phone disconnects, or who walks
// away: the only documented exit is an op the board can no longer receive, and
// the machine is bricked out of service with a "TEST MODE" screen and no way
// back. So there are three independent exits, and none of them may be removed
// without leaving a way to strand a board in the field:
//
//   1. the exit op (param 0) — the normal path;
//   2. 60 s with no command received (TEST_MODE_IDLE_TIMEOUT_MS) — covers a
//      dropped connection and a walk-away. Every command resets it, so a
//      technician actively working never trips it;
//   3. BTN1+BTN2 long press — needs no phone and no tools, for a board whose
//      radio is the thing that is broken.
//
// And it is RAM-only: nothing about test mode is persisted, so a reboot always
// comes back in normal operation. Do not "improve" that by remembering it
// across a restart — a board that boots into test mode cannot be recovered by
// power-cycling it, which is the one thing every field technician tries first.
//
// THE USER BUTTON (START, J1 pin 1 / IO18) is counted for free the whole time
// test mode is active - it is the counterpart to test_user_led on J1 pin 2, and
// verifying that harness means watching the lamp and the button together. Both
// counters share the LCD's second row. BTN1+BTN2 stays the exit gesture, so
// only START is repurposed, and nothing else in this state reads it.
//
// COINS: the slot is INHIBITED on entry (a machine that is out of service
// should not take money), and app_request_test_coin_slot(true) turns it back on
// for coin-path verification. Pulses counted then are reported by
// app_test_pulses() (a snapshot diff of coin_get_count()) and, over BLE, as
// Live Counters' trailing test_amount field.
//
// Coins dropped during a test are NOT banked as credit and NOT recorded as
// earnings (changed 2026-09-19; the original design banked them and honoured
// them on exit). coin_set_test_capture() diverts them: a test coin is the
// technician's, a banked one was handed to the next real customer as free
// credit, and it inflated lifetime/today takings and, once spent, the Session
// Log the backend treats as revenue. The never-discard invariant in CLAUDE.md
// is about CUSTOMER money, and there is no customer in test mode. Consequence
// worth knowing: the physical coin still drops into the cash box, so the box
// will hold more than the recorded takings until the technician takes their
// test coins back out.
// =============================================================================

// How long with no command before test mode exits itself (user's decision,
// 2026-09-17). See exit 2 above.
#define TEST_MODE_IDLE_TIMEOUT_MS 60000u

// Enter (true) or leave (false) test mode. Entering is honoured only from IDLE
// — it must never abandon a paid session; leaving is honoured always.
void app_request_test_mode(bool on);

// Enable (true) or inhibit (false) the coin slot while in test mode. Enabling
// restarts app_test_pulses() from zero. Ignored outside test mode: the coin
// slot belongs to the vend state machine everywhere else.
void app_request_test_coin_slot(bool on);

// Coin pulses since the slot was last test-enabled. Meaningless outside test
// mode.
uint32_t app_test_pulses();

// Reset the user-button (START, J1 pin 1 / IO18) press counter to zero.
//
// NOTE WHAT THIS IS *NOT*: there is no "enable the button" here, because a
// button is an INPUT and there is nothing to drive. The press counter is ALWAYS
// live in test mode - nothing else reads START there, so no op is needed to
// make the test work at all. This only re-zeroes the count so a wizard step can
// say "press it three times" and read exactly three.
//
// That is deliberate: the op code carrying it is provisional (see A13), and a
// feature whose only entry point is an unratified code would stop working if
// the app team reassigns it. This way a refusal costs a convenience, not the
// test.
void app_request_test_button_reset();

// User-button presses counted since entering test mode, or since the last
// app_request_test_button_reset(). Meaningless outside test mode.
uint32_t app_test_button_presses();

// Reset the test-mode inactivity timer. Called for EVERY BLE command op, so any
// activity at all keeps the mode alive. Harmless outside test mode.
void app_test_mode_poke();
