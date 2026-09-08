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
