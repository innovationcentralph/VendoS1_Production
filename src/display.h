#pragma once
#include <Arduino.h>
#include "config.h"

// =============================================================================
// Display module — abstracts LCD 16x2 I2C and TM1637 4-digit 7-segment.
// The active module is selected by AppConfig::display_type (stored in NVS).
// All functions are no-ops when display_type == DISPLAY_NONE.
//
// This header is a verbatim port of the STM32 firmware src/display.h — the
// public API is unchanged, so every call site in app.cpp and session.cpp is
// identical across the two codebases. Only the implementation differs.
// =============================================================================

// Create the internal display queue.
// Call from setup() BEFORE any task that posts to it is created.
void display_early_init();

// Record which display module is configured.
// Call from APP_STATE_INIT after config_load().
void display_init(const AppConfig* cfg);

// FreeRTOS task body — owns all I2C/LCD writes.
// Create with xTaskCreatePinnedToCore(display_task_run, ...) in main.cpp setup().
void display_task_run(void* arg);

// Boot splash — "VENDOLABS PH" / "V<firmware version>". Call once from
// APP_STATE_INIT right after display_init(); the caller is responsible for
// holding it on screen (e.g. a short vTaskDelay) before showing the idle screen.
void display_show_boot();

// Waiting for coins — while coin_value_cents == 0 shows "INSERT COIN" + the
// configured price-per-credit prompt. Once coin_value_cents > 0, row 1 shows
// the running coin value (centavos); row 2 shows either the whole credits
// earned (press_to_start_per_credit ON) or the computed total session time
// (OFF) — see display_set_idle_pricing().
void display_show_idle(uint32_t coin_value_cents);

// Waiting for coins in OP_AUTO_START — no "press button" prompt, since the
// session fires automatically the instant credit is satisfied. Both args are
// in centavos (coin_value_cents so far, and coins_required * 100) — same
// money-based units as display_show_idle/display_show_ready — so progress
// reads correctly regardless of price_per_credit_cents, and resets properly
// once coin_consume_value_cents() bills a credit (unlike a raw pulse count,
// which never resets and is not in the same unit as coins_required anyway).
void display_show_idle_auto(uint32_t coin_value_cents, uint32_t required_cents);

// Coins met threshold — same row layout as display_show_idle (coin_value_cents
// is always > 0 here, so this never shows the price prompt).
void display_show_ready(uint32_t coin_value_cents);

// Session countdown — call repeatedly; display updates at most once per second.
// Call with time_remaining_ms = total_ms before the loop starts to show initial value.
void display_show_session(uint32_t remaining_ms);

// Session finished ("Complete / Done").
void display_show_done();

// Start button pressed with insufficient credit — shows "INSUFFICIENT" / "CREDIT" briefly.
void display_show_denied();

// (OP_PRESS_TO_START or OP_PAUSE_RESUME) + press_to_start_per_credit only: one
// credit block just finished — shows how many are left, waiting for the next
// button press.
void display_show_next_credit(uint32_t remaining_credits);

// Config page — shows current relay_on_ms with -/+ button hints.
void display_show_config(uint32_t relay_on_ms);

// Config page — shows current coins_required (minimum-to-start / block size).
void display_show_config_credits(uint32_t coins_required);

// Config page — shows current operation_mode (OperationMode enum).
void display_show_config_opmode(uint8_t operation_mode);

// Config page (OP_PRESS_TO_START or OP_PAUSE_RESUME only) — shows press_to_start_per_credit.
void display_show_config_per_credit(bool enabled);

// Config page (OP_PRESS_TO_START or OP_PAUSE_RESUME only) — shows inactivity_timeout_s
// (0 = disabled, >0 = seconds before auto-start/auto-advance).
void display_show_config_inactivity_timeout(uint16_t timeout_s);

// Config page — shows accumulation_enabled (composable with any operation_mode).
void display_show_config_accumulation(bool enabled);

// Config page — shows sensor_stop_enabled (composable with any operation_mode).
void display_show_config_sensor_enable(bool enabled);

// Config page — shows sensor_active_high polarity.
void display_show_config_sensor_polarity(bool active_high);

// Config page — shows sensor_wait_ms (0 = immediate, >0 = required continuous-active time).
void display_show_config_sensor_wait(uint16_t wait_ms);

// Config page — shows price_per_credit_cents, the peso value of ONE raw
// pulse from whatever acceptor is wired in (coin acceptor: 1 pulse per peso;
// bill acceptor: pulses per peso configurable). Used only to convert pulses
// into the running "COIN:X.XX" money total (see coin_get_value_cents()) —
// it does NOT affect the idle screen "X.XX PER CREDIT" price, which is
// just coins_required (see display_set_idle_pricing()).
void display_show_config_price(uint32_t price_cents);

// Feeds the idle screen (display_show_idle/display_show_ready) everything it
// needs beyond the live credit count:
//   - credits == 0: "X.XX PER CREDIT" prompt = coins_required directly (the
//     number of pulses this machine costs to operate, shown as pesos).
//   - credits >  0: row 2 shows either the raw credit count (per_credit_mode
//     true, (OP_PRESS_TO_START or OP_PAUSE_RESUME) + press_to_start_per_credit)
//     or the computed total session time (false):
//     floor(credits / coins_required) * relay_on_ms.
// Call once after config_load() and again after any config-menu save so the
// idle screen reflects the live values.
void display_set_idle_pricing(uint32_t coins_required, uint32_t relay_on_ms, bool per_credit_mode);

// Diagnostic snapshot for AT+LCD? — whether the panel is considered healthy,
// its I2C address, and how many recoveries have been attempted.
void display_print_status(Stream& out);
