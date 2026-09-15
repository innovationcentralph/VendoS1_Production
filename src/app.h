#pragma once

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
} AppState;

// FreeRTOS task entry point — create this task in main.cpp setup().
void app_task_run(void* arg);

// ESP32/S1 ONLY — no STM32 counterpart, see docs/PORTING_FROM_STM32.md 2.5a.
//
// True when the board is not in the middle of anything a customer paid for or
// an operator is editing: IDLE only. Exists for the BLE Command characteristic
// (6a40f007), which must refuse a relay or buzzer test during a paid session —
// a relay click mid-vend is a customer complaint, and a test beep reads as a
// fault. Safe from any task: it reads a single aligned word published by the
// app task once per iteration of its state machine.
bool app_state_is_idle();

// Called by the app task itself, once per state-machine iteration. Not for
// anyone else to call.
void app_publish_state(AppState st);
