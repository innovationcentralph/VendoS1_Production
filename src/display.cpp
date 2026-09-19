#include "display.h"
#include "pins.h"
#include "version.h"
#include "lcd.h"

#include <Wire.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// Architecture note — unchanged from the STM32 firmware
// =============================================================================
// All I2C / LCD writes are confined to display_task_run().
// Callers (app_task, session_run) use display_show_*() which post a small
// message to a FreeRTOS queue and return immediately — they never block on I2C.
// If the LCD stalls (bus held low, clock-stretch timeout, etc.) only the display
// task freezes; the relay timer and session countdown are unaffected.
//
// That separation matters more here than it did on the STM32. There, a wedged
// display task cost you the screen. Here the watchdog window is ~100 ms, so a
// blocking I2C call on the app task would not merely freeze the display — it
// would reboot the board mid-vend, and the fault would look like a hardware
// problem. See src/wdt.h.
// =============================================================================

#define LCD_I2C_ADDR  0x27   // PCF8574 backpack default (0x3F is the other strap;
                             // lcd_init(0) autodetects both, so this is only the
                             // preferred first guess)

// =============================================================================
// Internal queue message
// =============================================================================
typedef enum : uint8_t {
    DISP_BOOT,
    DISP_IDLE,
    DISP_READY,
    DISP_IDLE_AUTO,
    DISP_SESSION,
    DISP_DONE,
    DISP_CONFIG,
    DISP_CONFIG_CREDITS,
    DISP_CONFIG_OPMODE,
    DISP_CONFIG_PER_CREDIT,
    DISP_CONFIG_INACTIVITY_TIMEOUT,
    DISP_NEXT_CREDIT,
    DISP_CONFIG_ACCUMULATION,
    DISP_CONFIG_SENSOR_ENABLE,
    DISP_CONFIG_SENSOR_POLARITY,
    DISP_CONFIG_SENSOR_WAIT,
    DISP_DENIED,
    DISP_CONFIG_PRICE,
    DISP_TEST_MODE,
} DispType;

typedef struct {
    DispType type;
    uint32_t a;   // credits / remaining_ms / relay_on_ms
    uint32_t b;   // required (IDLE / READY only)
    uint32_t c;   // TEST_MODE only: button presses. Added rather than packed
                  // into the spare bits of b - this struct is private to this
                  // file and 4 bytes x a depth-8 queue is not worth a bitfield
                  // nobody would remember the layout of.
} DispMsg;

// =============================================================================
// Module state — all written once at init, then read-only or display-task-only
// =============================================================================
static QueueHandle_t     s_disp_queue        = nullptr;
static uint8_t           s_type              = DISPLAY_NONE;

// Idle-screen billing config — written by display_set_idle_pricing() (app
// task), read only by display_task_run — same eventual-consistency treatment
// as s_type above.
static uint32_t          s_idle_coins_required  = 1;      // credits==0 price prompt + block size for the TIME calc
static uint32_t          s_idle_relay_on_ms     = 0;      // per-block duration, for the TIME calc
static bool              s_idle_per_credit_mode = false;  // true: row2=CREDIT count, false: row2=computed TIME

// Only ever read/written inside display_task_run — no mutex needed.
static uint32_t          s_session_last_secs = UINT32_MAX;

// Recovery state — display task only
static uint8_t           s_recovery_fails    = 0;
static uint16_t          s_recovery_count    = 0;
static bool              s_lcd_ok            = false;
static bool              s_lcd_started       = false;  // true after first lcd_hw_init()

// Last idle/ready state — used by the no-activity heartbeat to re-render after
// a reinit. s_idle_credits holds coin_value_cents for both the plain IDLE/READY
// screens and the OP_AUTO_START variant (which also uses s_idle_required, the
// required cents threshold, unused otherwise) — see s_idle_is_auto below.
static uint32_t          s_idle_credits      = 0;
static uint32_t          s_idle_required     = 0;
static bool              s_idle_is_auto      = false;  // true if the last idle screen shown was the OP_AUTO_START variant

// Last TEST MODE screen, for the same heartbeat. Test mode only posts a frame
// when its pulse or button count CHANGES, so a quiet stretch (a relay or
// buzzer test, the technician deciding Yes/No) routinely passes 5 s with no
// message. The heartbeat used to redraw IDLE unconditionally at that point,
// wiping the TEST MODE banner off a board that was still out of service and
// never putting it back. Set on DISP_TEST_MODE, cleared by any other screen.
static bool              s_test_active       = false;
static uint32_t          s_test_pulses       = 0;
static bool              s_test_slot_on      = false;
static uint32_t          s_test_presses      = 0;

// =============================================================================
// I2C bus recovery — called ONLY from display_task_run
// =============================================================================
// Motor noise can corrupt the bus in three ways. The failure modes are the same
// as on the STM32; only the detection and the unlock differ, because the ESP32
// exposes no I2C status registers to poke:
//
//   1. Slave stuck      — the backpack received a partial byte and holds SDA
//                         LOW. Fix: 9 SCL bit-bang pulses to complete the byte.
//                         Unchanged from the STM32.
//   2. Peripheral latch — the STM32 could get a phantom START latched into its
//                         I2C BUSY bit, cleared with I2C_CR1.SWRST. The ESP32
//                         has no equivalent exposed, so the stand-in is
//                         Wire.end() + Wire.begin(), which tears the peripheral
//                         down and rebuilds it.
//   3. Error flags      — the STM32 read BERR/ARLO/AF out of I2C1->SR1. Here the
//                         driver reports NAKs directly (lcd_write_failures()),
//                         which is better information: it says what the
//                         peripheral actually did rather than what the bus
//                         looks like.

// Initialize (or reinitialize) Wire and the LCD. Called at task startup and
// after a successful bus recovery.
static void lcd_hw_init() {
    lcd_bus_begin();
    if (lcd_init(LCD_I2C_ADDR)) {
        s_lcd_ok = true;
    } else {
        // Preferred address silent — sweep for it. On a daisy-chain this can
        // land on a sibling board, so say so.
        s_lcd_ok = lcd_init(0);
        if (s_lcd_ok && lcd_addr() != 0x27 && lcd_addr() != 0x3F) {
            Serial.print("[disp] WARN: using 0x");
            Serial.print(lcd_addr(), HEX);
            Serial.println(" — not a usual backpack address, may be another board");
        }
    }
    // Only touch the panel if one actually answered. With no device found,
    // lcd_addr() is still 0, and writing to address 0 is the I2C general-call
    // address — a broadcast to every device on a bus this board shares with its
    // daisy-chained siblings. Not something to emit just because the LCD is absent.
    if (s_lcd_ok) lcd_backlight(true);
    lcd_clear_write_failures();
    s_session_last_secs = UINT32_MAX; // force full session-row redraw on next DISP_SESSION
    s_recovery_fails    = 0;
}

static bool i2c_needs_recovery() {
    // Physical line check: SDA held LOW by a stuck slave. Read the pad directly
    // — this is the one check that ports over unchanged, because it inspects the
    // wire rather than the peripheral.
    if (digitalRead(PIN_LCD_SDA) == LOW) return true;

    // Driver-reported NAKs. Replaces the STM32 SR1 error-flag read; see the
    // block comment above. Cleared here so each check reports only new failures.
    if (lcd_write_failures() > 0) {
        lcd_clear_write_failures();
        return true;
    }
    return false;
}

// Unlock the I2C bus: 9 SCL bit-bang pulses + STOP condition.
// Does NOT call lcd_bus_begin() or lcd_init() — caller is responsible for those.
// Returns true if the bus looks clean afterward (SDA HIGH).
static bool i2c_bus_unlock() {
    lcd_bus_end();

    // 9 SCL pulses to clock out any partial byte a stuck slave is holding.
    pinMode(PIN_LCD_SCL, OUTPUT);
    digitalWrite(PIN_LCD_SCL, HIGH);
    pinMode(PIN_LCD_SDA, INPUT_PULLUP);
    for (uint8_t i = 0; i < 9; ++i) {
        if (digitalRead(PIN_LCD_SDA) == HIGH) break;
        digitalWrite(PIN_LCD_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(PIN_LCD_SCL, HIGH); delayMicroseconds(5);
    }

    // STOP condition: SCL low -> SDA low -> SCL high -> SDA high.
    pinMode(PIN_LCD_SDA, OUTPUT);
    digitalWrite(PIN_LCD_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_LCD_SDA, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_LCD_SCL, HIGH); delayMicroseconds(5);
    digitalWrite(PIN_LCD_SDA, HIGH); delayMicroseconds(5);

    // Release both lines to check actual bus state (not our own driven output).
    pinMode(PIN_LCD_SCL, INPUT_PULLUP);
    pinMode(PIN_LCD_SDA, INPUT_PULLUP);
    delayMicroseconds(10);

    ++s_recovery_count;
    return (digitalRead(PIN_LCD_SDA) == HIGH);
}

// Session-safe bus recovery: unlock + Wire re-begin, but NO full lcd_init().
// Called during an active session when i2c_needs_recovery() fires.
// Fast (~3 ms). The PCF8574 does not require re-initialisation — its I2C slave
// state is reset by the STOP condition; the LCD DDRAM is preserved.
static void i2c_session_recover() {
    Serial.println("[disp] session I2C recover + HD44780 reinit");
    i2c_bus_unlock();           // releases stuck slave
    lcd_bus_begin();            // re-enables the I2C peripheral on SDA/SCL
    lcd_clear_write_failures();
}

// Full recovery used on IDLE (relay off): unlock + Wire re-begin + full lcd_init.
static void i2c_recover_and_reinit() {
    Serial.println("[disp] I2C stuck — recovering");

    if (!i2c_bus_unlock()) {
        ++s_recovery_fails;
        if (s_recovery_fails >= 3) {
            s_lcd_ok = false;
            Serial.println("[disp] LCD disabled after 3 failed recoveries");
        } else {
            Serial.print("[disp] recovery failed (");
            Serial.print(s_recovery_fails);
            Serial.println("/3)");
        }
        return;
    }

    lcd_hw_init();
    Serial.println("[disp] I2C recovery complete");
}

static void lcd_health_check() {
    if (s_lcd_ok && i2c_needs_recovery()) i2c_recover_and_reinit();
}

// =============================================================================
// LCD helpers — called ONLY from display_task_run
// =============================================================================
static void lcd_row(uint8_t row, const char* text) {
    lcd_set_cursor(0, row);
    uint8_t i = 0;
    for (; i < 16 && text[i]; ++i) lcd_write_char(text[i]);
    for (; i < 16; ++i)            lcd_write_char(' ');
}

// Centers `label` (<=14 chars) within columns 1-14, with `left_ch` fixed at
// column 0 and `right_ch` fixed at column 15 — matches the B1/B2 button
// positions either side of the screen, so the selection/adjustment is
// spatially tied to the buttons that change it instead of printed "B1"/"B2"
// labels. Defaults to '<'/'>' (cycling a fixed set of choices); pass '-'/'+'
// for a numeric adjuster instead.
static void lcd_row_bracketed(uint8_t row, const char* label, char left_ch = '<', char right_ch = '>') {
    char buf[17];
    size_t len = strlen(label);
    if (len > 14) len = 14;   // truncate defensively — callers keep labels <= 14 chars
    const size_t pad_left  = (14 - len) / 2;
    const size_t pad_right = 14 - len - pad_left;

    size_t i = 0;
    buf[i++] = left_ch;
    for (size_t p = 0; p < pad_left; ++p)  buf[i++] = ' ';
    memcpy(&buf[i], label, len); i += len;
    for (size_t p = 0; p < pad_right; ++p) buf[i++] = ' ';
    buf[i++] = right_ch;
    buf[i] = '\0';

    lcd_row(row, buf);
}

// Centers `text` (<=16 chars) across the full row width — no brackets, used
// for page headers. Same lean-left tie-break as lcd_row_bracketed: when the
// padding cannot split evenly, the left side gets fewer spaces.
static void lcd_row_centered(uint8_t row, const char* text) {
    char buf[17];
    size_t len = strlen(text);
    if (len > 16) len = 16;   // truncate defensively
    const size_t pad_left = (16 - len) / 2;

    size_t i = 0;
    for (size_t p = 0; p < pad_left; ++p) buf[i++] = ' ';
    memcpy(&buf[i], text, len); i += len;
    buf[i] = '\0';

    lcd_row(row, buf);   // lcd_row pads any remaining columns with spaces
}

// Formats a whole-second duration as "Ns" below 60 s, or "Mmin Ss" at 60 s
// and above (e.g. 59 -> "59s", 60 -> "1min 0s") — shared by any screen whose
// displayed time can reach into minutes (config pages, idle/ready TIME row,
// and the dispensing countdown). Lowercase/abbreviated to save the limited
// 16-column LCD width.
static void format_duration_label(uint32_t total_secs, char* out, size_t out_size) {
    if (total_secs < 60) {
        snprintf(out, out_size, "%lus", (unsigned long)total_secs);
    } else {
        const uint32_t mins = total_secs / 60;
        const uint32_t secs = total_secs % 60;
        snprintf(out, out_size, "%lumin %lus", (unsigned long)mins, (unsigned long)secs);
    }
}

// Billing model (money-first, see display_set_idle_pricing()):
//   Credit           = floor(Coins Inserted / Coins per Credit) — leftover
//                      cents simply stay banked in coin_value_cents for the
//                      next session, never discarded.
//   Time to Dispense = Credit * Time per Credit (relay_on_ms)
//   Coins Inserted   = raw pulses * Price per Pulse (tracked in periph.cpp)
// Price Per Pulse therefore only ever affects Coins Inserted — it plays no
// further part in this function.
//
// coin_value_cents == 0: "INSERT COIN" + the price of ONE credit, shown as
// coins_required directly (e.g. coins_required=5 -> "5.00 PER CREDIT").
// coin_value_cents >  0: row 1 = running coin value; row 2 = whole Credits
//   earned (per_credit_mode) or the computed Time to Dispense for those
//   credits (otherwise).
static void lcd_render_boot() {
    lcd_row_centered(0, "VENDOLABS PH");
    char row1[17];
    snprintf(row1, sizeof(row1), "V%s", FW_VERSION_STRING);
    lcd_row_centered(1, row1);
}

static void lcd_render_idle(uint32_t coin_value_cents) {
    if (coin_value_cents == 0) {
        char row1[17];
        snprintf(row1, sizeof(row1), "%lu.00 PER CREDIT", (unsigned long)s_idle_coins_required);
        lcd_row(0, "  INSERT COIN   ");
        lcd_row(1, row1);
        return;
    }

    char row0[17];
    snprintf(row0, sizeof(row0), "COIN:%lu.%02lu",
             (unsigned long)(coin_value_cents / 100), (unsigned long)(coin_value_cents % 100));
    lcd_row(0, row0);

    // Whole credits earned (floor), same formula as APP_STATE_COIN_VALIDATE —
    // e.g. coins_required=5 (=P5.00/credit), coin_value_cents=1000 (P10.00)
    // -> 2 credits, with any leftover cents remaining banked.
    const uint32_t price_cents = s_idle_coins_required * 100;
    const uint32_t num_blocks  = (price_cents > 0) ? (coin_value_cents / price_cents) : 0;
    char row1[17];
    if (s_idle_per_credit_mode) {
        snprintf(row1, sizeof(row1), "CREDIT:%lu", (unsigned long)num_blocks);
    } else {
        const uint32_t total_secs = (num_blocks * s_idle_relay_on_ms) / 1000;
        char time_label[15];
        format_duration_label(total_secs, time_label, sizeof(time_label));
        snprintf(row1, sizeof(row1), "TIME:%s", time_label);
    }
    lcd_row(1, row1);
}

// ready implies coin_value_cents > 0 (coins_required >= 1), so this always
// takes the COIN-value branch above — reuse lcd_render_idle rather than
// duplicate it.
static void lcd_render_ready(uint32_t coin_value_cents) {
    lcd_render_idle(coin_value_cents);
}

static void lcd_render_idle_auto(uint32_t coin_value_cents, uint32_t required_cents) {
    lcd_row_centered(0, "AUTO DISPENSE");
    char row1[17];
    snprintf(row1, sizeof(row1), "%lu.%02lu/%lu.%02lu",
             (unsigned long)(coin_value_cents / 100), (unsigned long)(coin_value_cents % 100),
             (unsigned long)(required_cents / 100), (unsigned long)(required_cents % 100));
    lcd_row_centered(1, row1);
}

static void lcd_render_session(uint32_t remaining_ms) {
    const uint32_t secs = remaining_ms / 1000;

    // Throttle to once per second (avoid redundant I2C traffic).
    if (secs == s_session_last_secs) return;
    s_session_last_secs = secs;

    // Both rows refresh every second — heals mild DDRAM garble without a full reinit.
    lcd_row(0, "DISPENSING...");
    char time_label[15];
    format_duration_label(secs, time_label, sizeof(time_label));
    char row1[17];
    snprintf(row1, sizeof(row1), "TIME: %s", time_label);
    lcd_row(1, row1);
}

static void lcd_render_done() {
    lcd_row_centered(0, "SESSION");
    lcd_row_centered(1, "COMPLETE!");
}

static void lcd_render_config(uint32_t relay_on_ms) {
    const uint32_t secs = relay_on_ms / 1000;
    lcd_row_centered(0, "TIME PER CREDIT");
    char label[15];
    format_duration_label(secs, label, sizeof(label));
    lcd_row_bracketed(1, label, '-', '+');
}

static void lcd_render_config_credits(uint32_t coins_required) {
    lcd_row_centered(0, "COINS PER CREDIT");
    char label[15];
    snprintf(label, sizeof(label), "%lu", (unsigned long)coins_required);
    lcd_row_bracketed(1, label, '-', '+');
}

static void lcd_render_config_opmode(uint8_t operation_mode) {
    lcd_row_centered(0, "OPERATION MODE");
    switch (operation_mode) {
        case OP_PRESS_TO_START: lcd_row_bracketed(1, "PRESS TO START"); break;
        case OP_PAUSE_RESUME:   lcd_row_bracketed(1, "PAUSE & RESUME"); break;
        case OP_AUTO_START:     lcd_row_bracketed(1, "AUTO START");     break;
        default: {
            char label[15];
            snprintf(label, sizeof(label), "MODE %u", (unsigned)operation_mode);
            lcd_row_bracketed(1, label);
            break;
        }
    }
}

static void lcd_render_config_per_credit(bool enabled) {
    lcd_row_centered(0, "CREDIT MODE");
    // OFF = all billed blocks dispense in one continuous run (default),
    // ON = one block per button press; OP_PRESS_TO_START or OP_PAUSE_RESUME only
    lcd_row_bracketed(1, enabled ? "ON" : "OFF");
}

static void lcd_render_config_inactivity_timeout(uint16_t timeout_s) {
    lcd_row_centered(0, "INACTIVITY TIMER");
    char label[15];
    if (timeout_s == 0) {
        snprintf(label, sizeof(label), "DISABLE");
    } else {
        format_duration_label(timeout_s, label, sizeof(label));
    }
    lcd_row_bracketed(1, label, '-', '+');
}

static void lcd_render_next_credit(uint32_t remaining_credits) {
    lcd_row(0, " PRESS FOR NEXT ");
    char row1[17];
    snprintf(row1, sizeof(row1), "  CREDIT: %2u   ", (unsigned)remaining_credits);
    lcd_row(1, row1);
}

static void lcd_render_config_accumulation(bool enabled) {
    lcd_row_centered(0, "ACCUMULATION");
    // OFF = coins during a session are ignored until it ends, ON = coins add
    // time while running (composable with any operation_mode)
    lcd_row_bracketed(1, enabled ? "ON" : "OFF");
}

static void lcd_render_config_sensor_enable(bool enabled) {
    lcd_row_centered(0, "SENSOR STOP");
    // OFF = timer-only stop, ON = composable with any operation_mode
    lcd_row_bracketed(1, enabled ? "ON" : "OFF");
}

static void lcd_render_config_sensor_polarity(bool active_high) {
    lcd_row_centered(0, "SENSOR STOP CFG");
    // LOW = active-LOW (common switch/module default), HIGH = active-HIGH
    lcd_row_bracketed(1, active_high ? "HIGH" : "LOW");
}

static void lcd_render_config_sensor_wait(uint16_t wait_ms) {
    lcd_row_centered(0, "SENSOR WAIT TIME");
    char label[15];
    if (wait_ms == 0) {
        snprintf(label, sizeof(label), "IMMEDIATE");
    } else {
        snprintf(label, sizeof(label), "%uMS", (unsigned)wait_ms);
    }
    lcd_row_bracketed(1, label, '-', '+');
}

static void lcd_render_denied() {
    lcd_row_centered(0, "INSUFFICIENT");
    lcd_row_centered(1, "CREDIT");
}

static void lcd_render_test_mode(uint32_t pulses, bool slot_on, uint32_t presses) {
    lcd_row_centered(0, "TEST MODE");
    // Both counters on one row, because a technician verifying the J1 harness
    // is watching the coin line and the button at the same time and cannot page
    // a 16x2 display. "--" for an inhibited slot rather than "0": those are
    // different facts, and a zero would read as a coin path that is enabled and
    // not working. PULSES, not COINS - an acceptor set to P1/pulse emits five
    // pulses for a P5 coin, which is the whole basis of the billing model.
    char label[17];
    if (slot_on) {
        snprintf(label, sizeof(label), "PLS:%lu BTN:%lu",
                 (unsigned long)pulses, (unsigned long)presses);
    } else {
        snprintf(label, sizeof(label), "PLS:-- BTN:%lu", (unsigned long)presses);
    }
    lcd_row_centered(1, label);
}

static void lcd_render_config_price(uint32_t price_cents) {
    lcd_row_centered(0, "PRICE PER PULSE");
    char label[15];
    snprintf(label, sizeof(label), "%lu.%02lu",
             (unsigned long)(price_cents / 100), (unsigned long)(price_cents % 100));
    lcd_row_bracketed(1, label, '-', '+');
}

// =============================================================================
// Queue post helper — non-blocking, called from any task
// =============================================================================
static void disp_post(DispType type, uint32_t a, uint32_t b, uint32_t c = 0) {
    if (!s_disp_queue) return;
    const DispMsg msg = { type, a, b, c };
    xQueueSendToBack(s_disp_queue, &msg, 0);   // drop if full, never block caller
}

// =============================================================================
// Public API — init
// =============================================================================

void display_early_init() {
    s_disp_queue = xQueueCreate(8, sizeof(DispMsg));  // depth 8 — headroom during EMI recovery cycles
    if (!s_disp_queue) Serial.println("[disp] WARN: queue create failed");
}

void display_init(const AppConfig* cfg) {
    s_type = cfg->display_type;
    // Wire and LCD hardware init is deferred to display_task_run — it owns Wire
    // exclusively. Initializing from the app task (a different FreeRTOS context)
    // is unsafe because Wire is not re-entrant; display_task_run is the sole
    // user after startup.
    if (s_type == DISPLAY_LCD_16X2) {
        Serial.println("[disp] LCD 16x2 configured (autodetect 0x27/0x3F)");
    } else if (s_type == DISPLAY_7SEG_4DIG) {
        Serial.println("[disp] 7-seg stub — not implemented yet");
    } else {
        Serial.println("[disp] display disabled");
    }
}

// =============================================================================
// Public API — display_show_* (non-blocking queue posts)
// =============================================================================

void display_show_boot() {
    disp_post(DISP_BOOT, 0, 0);
}
void display_show_idle(uint32_t coin_value_cents) {
    disp_post(DISP_IDLE, coin_value_cents, 0);
}
void display_show_idle_auto(uint32_t coin_value_cents, uint32_t required_cents) {
    disp_post(DISP_IDLE_AUTO, coin_value_cents, required_cents);
}
void display_show_ready(uint32_t coin_value_cents) {
    disp_post(DISP_READY, coin_value_cents, 0);
}
void display_show_session(uint32_t remaining_ms) {
    disp_post(DISP_SESSION, remaining_ms, 0);
}
void display_show_done() {
    disp_post(DISP_DONE, 0, 0);
}
void display_show_config(uint32_t relay_on_ms) {
    disp_post(DISP_CONFIG, relay_on_ms, 0);
}
void display_show_config_credits(uint32_t coins_required) {
    disp_post(DISP_CONFIG_CREDITS, coins_required, 0);
}
void display_show_config_opmode(uint8_t operation_mode) {
    disp_post(DISP_CONFIG_OPMODE, operation_mode, 0);
}
void display_show_config_per_credit(bool enabled) {
    disp_post(DISP_CONFIG_PER_CREDIT, enabled ? 1u : 0u, 0);
}
void display_show_config_inactivity_timeout(uint16_t timeout_s) {
    disp_post(DISP_CONFIG_INACTIVITY_TIMEOUT, timeout_s, 0);
}
void display_show_config_accumulation(bool enabled) {
    disp_post(DISP_CONFIG_ACCUMULATION, enabled ? 1u : 0u, 0);
}
void display_show_config_sensor_enable(bool enabled) {
    disp_post(DISP_CONFIG_SENSOR_ENABLE, enabled ? 1u : 0u, 0);
}
void display_show_config_sensor_polarity(bool active_high) {
    disp_post(DISP_CONFIG_SENSOR_POLARITY, active_high ? 1u : 0u, 0);
}
void display_show_config_sensor_wait(uint16_t wait_ms) {
    disp_post(DISP_CONFIG_SENSOR_WAIT, wait_ms, 0);
}
void display_show_denied() {
    disp_post(DISP_DENIED, 0, 0);
}
void display_show_next_credit(uint32_t remaining_credits) {
    disp_post(DISP_NEXT_CREDIT, remaining_credits, 0);
}
void display_show_config_price(uint32_t price_cents) {
    disp_post(DISP_CONFIG_PRICE, price_cents, 0);
}

void display_show_test_mode(uint32_t pulses, bool slot_on, uint32_t presses) {
    disp_post(DISP_TEST_MODE, pulses, slot_on ? 1 : 0, presses);
}

void display_set_idle_pricing(uint32_t coins_required, uint32_t relay_on_ms, bool per_credit_mode) {
    s_idle_coins_required  = coins_required;
    s_idle_relay_on_ms     = relay_on_ms;
    s_idle_per_credit_mode = per_credit_mode;
}

void display_print_status(Stream& out) {
    out.print("display_type = "); out.println(s_type);
    if (s_type != DISPLAY_LCD_16X2) { out.println("LCD not selected"); return; }
    out.print("lcd state    = ");
    if (!s_lcd_started)  out.println("not started yet");
    else if (s_lcd_ok)   out.println("OK");
    else                 out.println("*** DISABLED after 3 failed recoveries ***");
    out.print("lcd addr     = 0x"); out.println(lcd_addr(), HEX);
    out.print("recoveries   = "); out.println(s_recovery_count);
    out.print("nak count    = "); out.println(lcd_write_failures());
}

// =============================================================================
// Display task — the ONLY code that calls Wire / LCD
// =============================================================================

void display_task_run(void* arg) {
    (void)arg;

    if (!s_disp_queue) {
        Serial.println("[disp] FATAL: no queue, display task exiting");
        vTaskDelete(nullptr);
        return;
    }

    Serial.println("[disp] task running");

    DispMsg msg;
    for (;;) {
        if (xQueueReceive(s_disp_queue, &msg, pdMS_TO_TICKS(5000)) != pdTRUE) {
            // 5 s with no messages — reinit the HD44780 and re-render idle to
            // clear any garble caused by relay-off EMI that did not trip an I2C
            // bus error. Re-render whichever idle variant was last shown (plain
            // vs OP_AUTO_START) so this heartbeat does not flicker back to the
            // wrong screen.
            if (s_type == DISPLAY_LCD_16X2 && s_lcd_ok && s_lcd_started) {
                lcd_reinit();
                if (s_test_active)       lcd_render_test_mode(s_test_pulses, s_test_slot_on, s_test_presses);
                else if (s_idle_is_auto) lcd_render_idle_auto(s_idle_credits, s_idle_required);
                else                     lcd_render_idle(s_idle_credits);
            }
            continue;
        }
        if (s_type == DISPLAY_NONE) continue;

        // Any screen other than TEST MODE means test mode is over (or never
        // started), so the heartbeat must go back to redrawing idle. Tracked
        // here, before the render switch, so it holds even when the LCD is
        // down and the switch below is skipped.
        if (msg.type == DISP_TEST_MODE) {
            s_test_active  = true;
            s_test_pulses  = msg.a;
            s_test_slot_on = (msg.b != 0);
            s_test_presses = msg.c;
        } else {
            s_test_active = false;
        }

        // Lazy one-time init: Wire/LCD setup is deferred to the first message
        // because s_type is set by display_init() in the app task (after
        // config_load), which only runs after the tasks are scheduled — too late
        // for a startup check at task entry where s_type is still DISPLAY_NONE.
        if (s_type == DISPLAY_LCD_16X2 && !s_lcd_started) {
            s_lcd_started = true;
            lcd_hw_init();
            Serial.print("[disp] LCD 16x2 I2C init (addr=0x");
            Serial.print(lcd_addr(), HEX);
            Serial.println(", 100kHz)");
        }

        // Reset s_session_last_secs on non-session transitions so the next
        // session first DISP_SESSION redraws the "DISPENSING..." header.
        if (msg.type == DISP_IDLE || msg.type == DISP_IDLE_AUTO || msg.type == DISP_DONE ||
            msg.type == DISP_DENIED || msg.type == DISP_NEXT_CREDIT) {
            s_session_last_secs = UINT32_MAX;
        }

        // Session path: fast bus recovery + HD44780 resync (~16 ms max).
        // Full reinit deferred to IDLE after relay-off to avoid the EMI window.
        if (s_type == DISPLAY_LCD_16X2 && msg.type == DISP_SESSION) {
            if (s_lcd_ok) {
                // If the PCF8574 is stuck (SDA low, or NAKs reported), release it
                // with a fast bus unlock before attempting the write.
                // i2c_session_recover() takes ~3 ms and does NOT run the full
                // power-on init, so it is safe while the relay (and motor EMI)
                // is active.
                if (i2c_needs_recovery()) {
                    i2c_session_recover();
                    lcd_reinit();  // resync HD44780 nibble counter after bus unlock
                    s_session_last_secs = UINT32_MAX;  // force full redraw on next write
                }
                lcd_render_session(msg.a);
            }
            continue;
        }

        if (s_type == DISPLAY_LCD_16X2 && (msg.type == DISP_IDLE || msg.type == DISP_IDLE_AUTO) && !s_lcd_ok) {
            s_recovery_fails = 0;
            i2c_recover_and_reinit();
        }

        if (s_type == DISPLAY_LCD_16X2) lcd_health_check();
        if (s_type == DISPLAY_LCD_16X2 && !s_lcd_ok) continue;

        switch (msg.type) {
            case DISP_BOOT:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_boot();
                break;
            case DISP_IDLE:
                if (s_type == DISPLAY_LCD_16X2) {
                    s_idle_credits  = msg.a;   // coin_value_cents
                    s_idle_is_auto  = false;
                    lcd_render_idle(msg.a);
                }
                break;
            case DISP_READY:
                if (s_type == DISPLAY_LCD_16X2) {
                    s_idle_credits  = msg.a;   // coin_value_cents
                    s_idle_is_auto  = false;
                    lcd_render_ready(msg.a);
                }
                break;
            case DISP_IDLE_AUTO:
                if (s_type == DISPLAY_LCD_16X2) {
                    s_idle_credits  = msg.a;
                    s_idle_required = msg.b;
                    s_idle_is_auto  = true;
                    lcd_render_idle_auto(msg.a, msg.b);
                }
                break;
            case DISP_SESSION:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_session(msg.a);
                break;
            case DISP_DONE:
                if (s_type == DISPLAY_LCD_16X2) {
                    lcd_reinit();   // resync HD44780 after the relay-off EMI window
                    lcd_render_done();
                }
                break;
            case DISP_CONFIG:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config(msg.a);
                break;
            case DISP_CONFIG_CREDITS:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_credits(msg.a);
                break;
            case DISP_CONFIG_OPMODE:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_opmode((uint8_t)msg.a);
                break;
            case DISP_CONFIG_PER_CREDIT:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_per_credit(msg.a != 0);
                break;
            case DISP_CONFIG_INACTIVITY_TIMEOUT:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_inactivity_timeout((uint16_t)msg.a);
                break;
            case DISP_NEXT_CREDIT:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_next_credit(msg.a);
                break;
            case DISP_CONFIG_ACCUMULATION:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_accumulation(msg.a != 0);
                break;
            case DISP_CONFIG_SENSOR_ENABLE:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_sensor_enable(msg.a != 0);
                break;
            case DISP_CONFIG_SENSOR_POLARITY:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_sensor_polarity(msg.a != 0);
                break;
            case DISP_CONFIG_SENSOR_WAIT:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_sensor_wait((uint16_t)msg.a);
                break;
            case DISP_DENIED:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_denied();
                break;
            case DISP_CONFIG_PRICE:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_config_price(msg.a);
                break;
            case DISP_TEST_MODE:
                if (s_type == DISPLAY_LCD_16X2) lcd_render_test_mode(msg.a, msg.b != 0, msg.c);
                break;
        }
    }
}
