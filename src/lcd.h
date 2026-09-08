#pragma once
#include <Arduino.h>

// =============================================================================
// 16x2 HD44780 LCD behind a PCF8574 I2C backpack.
//
// The STM32 firmware used the vendored lib/LiquidCrystal_I2C. This is a
// hand-rolled replacement rather than a straight copy, for two reasons:
//
//   1. It is the same driver the S1 bring-up harness proved on this exact board
//      (../Vendo_S1_TestCode/src/s1_i2c.cpp), including the PCF8574 bit mapping,
//      which is a convention rather than a standard and does vary between
//      backpacks. If the display lights up and shows garbage, that mapping at
//      the top of lcd.cpp is the first thing to change.
//
//   2. LiquidCrystal_I2C swallows the result of every Wire transaction. The
//      STM32 worked around that by reading I2C1 status registers directly to
//      spot a wedged bus (BERR/ARLO/AF/BUSY latch). The ESP32 exposes no
//      equivalent registers, so the ACK/NAK status has to come from the driver
//      itself — which is strictly better information anyway, since it reports
//      what the peripheral actually did rather than what the bus looks like.
//      lcd_write_failures() is what display.cpp uses in place of those register
//      reads; see i2c_needs_recovery() there.
//
// Every function here talks to Wire, so ALL of them are display-task-only —
// same exclusive-ownership rule the STM32 had.
// =============================================================================

#define LCD_COLS 16
#define LCD_ROWS 2

// Bring up Wire on the daisy-chain pins. Safe to call again after a bus unlock.
void lcd_bus_begin();

// Release Wire so the SDA/SCL pads can be bit-banged for a bus unlock.
void lcd_bus_end();

// Probe the two common backpack strap addresses (0x27, 0x3F), then fall back to
// the first device that ACKs anywhere in 0x08..0x77. Returns 0 if the bus is
// silent. The fallback is a guess on a daisy-chain — sibling boards answer too.
uint8_t lcd_autodetect();

// Full HD44780 power-on sequence in 4-bit mode. Blocking for ~65 ms because the
// datasheet waits are mandatory minimums and there is no status line to poll on
// a write-only backpack. Pass 0 to autodetect the address. Returns false if
// nothing ACKed.
//
// Safe to block this long only because the wdt task pets the TPL5010
// independently — 65 ms is most of a ~100 ms watchdog window. Do not try to pet
// from in here; see the note in lcd_autodetect() in lcd.cpp for why that makes
// things worse rather than better.
bool lcd_init(uint8_t addr);

// Re-run just the 4-bit entry sequence to resync the controller nibble counter,
// without the full power-on waits. This is the STM32 driver reinit() equivalent
// and is used in exactly the same places: after a bus unlock, after the relay-off
// EMI window, and on the display task no-message heartbeat.
void lcd_reinit();

void lcd_clear();
void lcd_set_cursor(uint8_t col, uint8_t row);
void lcd_write_char(char c);
void lcd_backlight(bool on);

bool    lcd_ready();
uint8_t lcd_addr();

// Count of I2C transactions the expander did not ACK since the last
// lcd_clear_write_failures(). Non-zero means the bus or the backpack needs
// recovering — this is the ESP32 stand-in for the STM32 SR1 error-flag check.
uint16_t lcd_write_failures();
void     lcd_clear_write_failures();
