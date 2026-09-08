#include "lcd.h"
#include "pins.h"
#include <Wire.h>

// --- PCF8574 -> HD44780 wiring on a generic "LCD1602 I2C" backpack.
// This is the near-universal mapping, but it is a convention, not a standard.
// If the display lights up and shows nothing but garbage, this block is the
// first thing to change.
#define LCD_RS 0x01
#define LCD_RW 0x02
#define LCD_EN 0x04
#define LCD_BL 0x08    // backlight transistor
// D4..D7 occupy the top nibble.

static uint8_t  s_addr        = 0;
static bool     s_ready       = false;
static bool     s_backlight   = true;
static uint16_t s_nak         = 0;

// =============================================================================
// Bus
// =============================================================================
void lcd_bus_begin() {
    Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL, 100000);   // 100 kHz — wider bit
                                                    // windows, more noise-immune,
                                                    // same rate the STM32 used
    // Without a timeout a bus held low by an unpowered peripheral — or by
    // unstuffed JP1/JP2 — stalls the transfer indefinitely. On the STM32 that
    // job was done by a DWT-cycle-counter hard timeout inside its vendored
    // twi.c. Here Wire does it for us, and it matters more: a stalled transfer
    // in the display task is survivable, but the same stall in a task that
    // stopped yielding would eventually be a watchdog reset.
    Wire.setTimeOut(50);
}

void lcd_bus_end() {
    Wire.end();
}

// =============================================================================
// Expander primitives
// =============================================================================
static bool expander_write(uint8_t v) {
    Wire.beginTransmission(s_addr);
    Wire.write((uint8_t)(v | (s_backlight ? LCD_BL : 0)));
    if (Wire.endTransmission() == 0) return true;
    ++s_nak;
    return false;
}

static void pulse_enable(uint8_t v) {
    expander_write((uint8_t)(v | LCD_EN));
    delayMicroseconds(1);           // EN pulse width >= 450 ns
    expander_write((uint8_t)(v & ~LCD_EN));
    delayMicroseconds(50);          // commands settle in ~37 us
}

static void write4(uint8_t v) {
    expander_write(v);
    pulse_enable(v);
}

// mode = 0 for a command, LCD_RS for character data.
static void lcd_send(uint8_t value, uint8_t mode) {
    write4((uint8_t)((value & 0xF0) | mode));
    write4((uint8_t)(((value << 4) & 0xF0) | mode));
}

// The 4-bit entry dance, shared by lcd_init() and lcd_reinit(). Three tries at
// 0x30 get the controller out of whichever of 8-bit or 4-bit mode it happens to
// be in after an incomplete previous init — which is exactly the state a bus
// unlock mid-nibble leaves it in.
static void enter_4bit_mode() {
    write4(0x30); delayMicroseconds(4500);
    write4(0x30); delayMicroseconds(4500);
    write4(0x30); delayMicroseconds(150);
    write4(0x20);               // and now settle on 4-bit
}

static void apply_display_defaults() {
    lcd_send(0x28, 0);          // function set: 4-bit, 2 lines, 5x8 font
    lcd_send(0x0C, 0);          // display on, cursor off, blink off
    lcd_send(0x06, 0);          // entry mode: increment, no shift
    lcd_send(0x01, 0);          // clear
    delayMicroseconds(2000);    // clear needs ~1.5 ms
}

// =============================================================================
// Init
// =============================================================================
uint8_t lcd_autodetect() {
    static const uint8_t kCommon[2] = { 0x27, 0x3F };
    for (uint8_t i = 0; i < 2; ++i) {
        Wire.beginTransmission(kCommon[i]);
        if (Wire.endTransmission() == 0) return kCommon[i];
    }
    // Fall back to whatever is on the bus. A vending daisy-chain may well have
    // sibling boards answering too, so this is a guess — display.cpp says so
    // on the log line when it lands here.
    // NOTE ON TIMING: with the bus held low — exactly the JP1/JP2-unstuffed
    // case — each probe below can burn the full 50 ms Wire timeout, so this
    // sweep can take seconds. That is safe here only because the wdt task pets
    // the TPL5010 independently at a higher priority (see src/wdt.h).
    //
    // Do NOT add wdt_pet() calls to this loop. The S1 bring-up harness had to,
    // because it petted from its main loop and had no such task; here it would
    // race the wdt task on IO2 and could cut a DONE pulse short of the
    // TPL5010 minimum width, which is a reset — the opposite of the intent.
    for (uint8_t a = 0x08; a <= 0x77; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) return a;
    }
    return 0;
}

bool lcd_init(uint8_t addr) {
    s_ready = false;

    if (addr == 0) {
        addr = lcd_autodetect();
        if (addr == 0) return false;
    }
    s_addr = addr;
    s_nak  = 0;

    if (!expander_write(0x00)) return false;   // nothing home at this address

    // HD44780 power-on sequence. The waits are datasheet minimums, not padding
    // — there is no status line to poll on a write-only backpack. Totals ~65 ms,
    // which is why the wdt task rather than this function owns petting: 65 ms of
    // mandatory blocking is most of a ~100 ms watchdog window, and nothing this
    // function could do about it would be safe (see the note in lcd_autodetect).
    delay(50);

    enter_4bit_mode();
    apply_display_defaults();

    s_ready = true;
    return true;
}

void lcd_reinit() {
    if (!s_addr) return;
    // No power-on wait here: the controller is already up, this only resyncs
    // the nibble counter and restores the display/entry mode.
    enter_4bit_mode();
    apply_display_defaults();
}

// =============================================================================
// Drawing
// =============================================================================
void lcd_clear() {
    lcd_send(0x01, 0);
    delayMicroseconds(2000);
}

void lcd_set_cursor(uint8_t col, uint8_t row) {
    // Row 1 lives at DDRAM 0x40 on a 16x2 — the classic off-by-a-bank trap.
    static const uint8_t kRowOffset[LCD_ROWS] = { 0x00, 0x40 };
    if (row >= LCD_ROWS) row = LCD_ROWS - 1;
    lcd_send((uint8_t)(0x80 | (col + kRowOffset[row])), 0);
}

void lcd_write_char(char c) {
    lcd_send((uint8_t)c, LCD_RS);
}

void lcd_backlight(bool on) {
    s_backlight = on;
    expander_write(0x00);   // push the new backlight bit out immediately
}

bool     lcd_ready()          { return s_ready; }
uint8_t  lcd_addr()           { return s_addr; }
uint16_t lcd_write_failures() { return s_nak; }
void     lcd_clear_write_failures() { s_nak = 0; }
