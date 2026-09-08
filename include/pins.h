#pragma once
#include <stdint.h>

// =============================================================================
// Vendo S1 Production Firmware — Pin Mapping
// Target: ESP32-WROOM-32 (Vendo S1 board)
//
// Transcribed from the schematic reference at
//   ../Vendo_I2C_Master_Slave/docs/hardware/ESP32_Vendo_Board.md
// and cross-checked against the bring-up harness pin map,
//   ../Vendo_S1_TestCode/src/s1_pins.h
// If the schematic and this file ever disagree, the schematic wins and this
// file is the bug.
//
// Names deliberately match the STM32 firmware include/pins.h so the two
// codebases stay diffable — PIN_RELAY means the same thing on both boards even
// though one is PA6 and the other IO19. GPIO numbers (not the WROOM-32 module
// 38-pad numbers) since that is what Arduino wants.
//
// -----------------------------------------------------------------------------
// STM32 -> S1 correspondence, and the four places it is NOT one-to-one
// -----------------------------------------------------------------------------
//   PIN_LED_A     PA2  -> (none)      S1 has only ONE debug LED. leds_set()
//                                     drives PIN_LED_B alone; see periph.cpp.
//   PIN_BTN3      PB15 -> (none)      S1 has only two config switches. The
//                                     whole BTN3 gesture set is remapped onto
//                                     BTN1+BTN2 and the START button — see the
//                                     "Config menu gestures" block below.
//   PIN_SENSOR_IN PB0  -> IO32        No dedicated sensor input exists on the
//                                     S1, so Sensor Stop reads AUX_1 on the J6
//                                     expansion header. The J6 rail is
//                                     selectable +3.3V/+5V via JP9 — match it
//                                     to the sensor.
//   (none)             -> IO2         NEW: TPL5010 external watchdog DONE.
//                                     The STM32 watchdog was the internal IWDG
//                                     with no pin at all. See src/wdt.h.
// =============================================================================

// -----------------------------------------------------------------------------
// Outputs — low-side MOSFET gates, all with a 10K pull-down.
// LOW = off is both the safe state and the powered-down state, so every one of
// these is written LOW *before* pinMode(OUTPUT) — see out_safe_low() in
// periph.cpp.
// -----------------------------------------------------------------------------
static const uint8_t PIN_LED_B      = 15;  // -> R30 1K -> D7 blue debug LED.
                                           //    STRAP (MTDO): low at reset silences the
                                           //    ROM boot log on U0TXD. Cosmetic, but R30
                                           //    + D7 share the node with the internal 45K
                                           //    boot pull-up, so which way it straps is a
                                           //    scope question. periph_reset_safe()
                                           //    releases it before a software reset.
static const uint8_t PIN_BUZZER     = 27;  // -> Q4 -> BZ1 (+5V). Driven by LEDC, not tone().
static const uint8_t PIN_RELAY      = 19;  // -> Q2 -> K1 relay coil (+5V), active-HIGH
static const uint8_t PIN_USER_LED   = 23;  // -> Q1, sinks the J1 illuminated button LED

// Coin slot control (active-HIGH enable) — a genuine coin-acceptor inhibit.
// HIGH = coin slot enabled (accepting coins). LOW = inhibited (coins blocked).
// Same polarity as the STM32 PA4, so the call sites port unchanged.
static const uint8_t PIN_COIN_EN    = 12;  // -> Q3 gate
                                           //    STRAP (flash voltage select): HIGH at
                                           //    reset can force 1.8V flash timing and stop
                                           //    the board booting. R21 10K pull-down
                                           //    covers power-on; periph_reset_safe()
                                           //    covers a software reset. NEVER idle high.

// -----------------------------------------------------------------------------
// Watchdog — TPL5010 (U5), external and unmaskable. See src/wdt.h.
// -----------------------------------------------------------------------------
static const uint8_t PIN_WDT_DONE   = 2;   // -> TPL5010 DONE (pet input)
                                           //    STRAP: high at reset picks an unintended
                                           //    boot mode, so this is driven low first
                                           //    thing in setup() and never idles high.

// -----------------------------------------------------------------------------
// Buttons — all active-LOW. Every one has its OWN external pull-up, so pinMode
// is plain INPUT, never INPUT_PULLUP.
// -----------------------------------------------------------------------------
static const uint8_t PIN_USER_BTN   = 18;  // <- J1 pin1 (illuminated button), R10 1K pull-up
static const uint8_t PIN_BTN1       = 34;  // <- SW2 (BTN_MODE_1), R13 10K pull-up
static const uint8_t PIN_BTN2       = 35;  // <- SW3 (BTN_MODE_2), R16 10K pull-up

// IO34/IO35 are input-only pads: no output driver and no internal pull-up or
// pull-down exists on them at all. Correct for switches with their own external
// 10K — but it means pinMode() may only ever be INPUT for these two.

// -----------------------------------------------------------------------------
// Config menu gestures — the S1 has two config buttons where the STM32 had
// three (BTN1/BTN2/BTN3), so the BTN3 jobs are redistributed. This is the ONLY
// user-facing behavioural difference between the two firmwares.
//
//   IDLE   BTN1 + BTN2 long press (~1.5 s)  -> enter config mode
//                                              (STM32: BTN3 long press)
//          START tap                        -> begin session (unchanged)
//
//   MENU   BTN1 = decrement / prev          (unchanged, hold to auto-repeat)
//          BTN2 = increment / next          (unchanged, hold to auto-repeat)
//          START tap                        -> next page, wraps
//                                              (STM32: BTN3 short press)
//          START long press (~1.5 s)        -> CANCEL, discard every change
//                                              (STM32: BTN1+BTN2 long press)
//          BTN1 + BTN2 long press (~1.5 s)  -> SAVE all pages, exit
//                                              (STM32: BTN3 long press)
//
// START carries both a short and a long meaning inside the menu, so the menu
// uses the release-vs-hold entry points (start_button_short_released() /
// start_button_long_pressed()) rather than the press-confirm one the IDLE path
// uses. There is no ambiguity with starting a session: entering the menu
// requires the BTN1+BTN2 combo, so a START tap can never mean both things.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Digital inputs
// -----------------------------------------------------------------------------
static const uint8_t PIN_COIN_IN    = 17;  // <- PC817 (U4) opto, coin pulse.
                                           //    PULSE POLARITY IS NOT IN THE SCHEMATIC.
                                           //    Unlike the STM32 (idle LOW, pulse HIGH,
                                           //    hard-coded), this is a config field:
                                           //    AppConfig::coin_active_high, settable with
                                           //    AT+COIN_POLARITY. Default is active-LOW,
                                           //    matching the opto-output-with-pull-up
                                           //    shape the bring-up harness assumes.

// Sensor Stop input — see the correspondence table above for why this is an AUX
// pin. Polarity and timing come from AppConfig, exactly as on the STM32.
static const uint8_t PIN_SENSOR_IN  = 32;  // <- J6 AUX_1
static const uint8_t PIN_AUX_2      = 33;  // <- J6 AUX_2, unused by this firmware

// Board role strap — JP10. Open = HIGH via R17 100K, jumper 1-2 = LOW.
// Read-only: driving it would fight the jumper. Reported by AT+STRAP? but not
// acted on — the master/slave split is the next phase (see docs/PENDING.md).
static const uint8_t PIN_I2C_CONF   = 5;   // <- JP10 (I2C_CONFG). STRAP (SDIO timing only).

// -----------------------------------------------------------------------------
// Display / daisy-chain I2C bus (J3/J4)
// -----------------------------------------------------------------------------
// The 16x2 LCD on its PCF8574 backpack shares the daisy-chain bus. Bus pull-ups
// R2/R3 4.7K are gated by solder jumpers JP1/JP2 — exactly one board in a chain
// should stuff them. Unstuffed everywhere is a bus that never rises, which looks
// identical to a dead display.
static const uint8_t PIN_LCD_SDA    = 21;  // ESP_SDA
static const uint8_t PIN_LCD_SCL    = 22;  // ESP_SCL

// Wired-OR dispense-request line — counterpart of the STM32 rig PB5. Reserved
// for the master/slave phase; this firmware leaves it an input and never drives
// it. Open-drain ONLY when that lands: two boards driving it push-pull is a
// direct short.
static const uint8_t PIN_I2C_INT    = 16;  // R32 100K pull-up + C29 100nF (~7 ms rise)

// -----------------------------------------------------------------------------
// Deferred / not owned by this firmware
// -----------------------------------------------------------------------------
// 7-segment module on J9 — DISPLAY_7SEG_4DIG was a stub on the STM32 too. Left
// defined so the pins cannot be reused by accident. TM1637-style is only an
// inference from the 2-wire CLK/DIO shape; the module type is unconfirmed.
static const uint8_t PIN_7SEG_CLK   = 13;  // SEG_CLK
static const uint8_t PIN_7SEG_DIO   = 14;  // SEG_DIO

// RTC U6 (SLM1302, DS1302-family 3-wire) — capability the STM32 board does not
// have, so there is nothing to port. Untouched by this firmware.
static const uint8_t PIN_RTC_CE     = 25;
static const uint8_t PIN_RTC_CLK    = 26;
static const uint8_t PIN_RTC_DAT    = 4;

// Not GPIOs we own: IO0 (BOOT button SW7, strap), IO1/IO3 (UART0 -> Serial, the
// J7 PROG header — the STM32 Serial1/PA9-PA10 equivalent), IO36/IO39 (unused,
// not broken out), IO6..IO11 (WROOM-32 internal SPI flash). ESP_EN is a pin,
// not a GPIO — SW1, R12 10K and the TPL5010 nRST all wire-AND onto it.
