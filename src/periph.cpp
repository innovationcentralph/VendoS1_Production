#include "periph.h"
#include "counters.h"
#include "diag.h"
#include "debug_log.h"
#include "wdt.h"

// LEDC (buzzer tone) moved from channel handles to pin handles between
// Arduino-ESP32 2.x and 3.x. Both spellings are kept so this builds on
// whichever core the toolchain resolves — same guard the S1 bring-up harness uses.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define S1_LEDC_V3 1
#else
  #define S1_LEDC_V3 0
  #define S1_BUZZ_CH 0
#endif

// =============================================================================
// periph_init — configure all GPIO directions and safe initial states
// =============================================================================

// Bring a MOSFET gate up as an output without ever letting it glitch high.
// Order matters: the level is written before pinMode() so the pad output
// register already holds 0 the moment the driver is enabled. All the gates have
// a 10K pull-down, and IO12 (COIN_EN) is additionally a flash-voltage strapping
// pin, so a glitch there is not merely a stray click.
static void out_safe_low(uint8_t pin) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

void periph_init() {
    // Outputs — drive LOW (safe off) at boot.
    // The STM32 also enabled its DWT cycle counter here, as a SysTick-independent
    // I2C timeout source for its vendored twi.c. Nothing to port: Wire.setTimeOut()
    // covers that on the ESP32 (see lcd_bus_begin()).
    out_safe_low(PIN_LED_B);
    out_safe_low(PIN_BUZZER);
    out_safe_low(PIN_RELAY);
    out_safe_low(PIN_USER_LED);

    // Coin slot enable — HIGH at boot so coins are always accepted until a
    // session says otherwise. Note this differs from the bring-up harness, which
    // left the acceptor inhibited by default: this is product firmware, and an
    // idle machine that will not take money is a broken machine.
    out_safe_low(PIN_COIN_EN);
    digitalWrite(PIN_COIN_EN, HIGH);

    // Inputs — active-LOW, but every one has its OWN external pull-up, so plain
    // INPUT and never INPUT_PULLUP. (The STM32 used INPUT_PULLUP throughout
    // because it had no external pull-ups; IO34/IO35 here have no internal
    // pull-up available at all, so this is forced as well as correct.)
    pinMode(PIN_BTN1,     INPUT);   // SW2, R13 10K
    pinMode(PIN_BTN2,     INPUT);   // SW3, R16 10K
    pinMode(PIN_USER_BTN, INPUT);   // J1,  R10 1K

    // Coin pulse input. Plain INPUT like every other input on this board: R25 is
    // a hard 1K external pull-up from +3.3V onto COIN_IN, so the internal ~45K
    // adds nothing it can win. (This line was INPUT_PULLUP, justified by the
    // pre-2026-09-14 belief that the pulse was active-LOW and the line needed
    // holding high at idle. The schematic says the opposite — U4 conducts at
    // idle and pulls COIN_IN down; see AppConfig::coin_active_high — so that
    // reasoning was backwards, and the internal pull-up was fighting the opto
    // rather than helping it.)
    pinMode(PIN_COIN_IN,  INPUT);

    // Board-ID strap — read-only. Driving it would fight the JP10 jumper.
    pinMode(PIN_I2C_CONF, INPUT);

    // Sensor input (Sensor Stop) — polarity resolved in sensor_read_active().
    // On J6 AUX_1; pull-up kept for the common dry-contact-to-ground sensor,
    // which is also what the active-LOW default assumes.
    pinMode(PIN_SENSOR_IN, INPUT_PULLUP);

    // Reserved for the master/slave phase — left as an input so nothing drives
    // the wired-OR line. See docs/PENDING.md.
    pinMode(PIN_I2C_INT, INPUT);
}

void periph_reset_safe() {
    // Relay first — the point of the exercise.
    digitalWrite(PIN_RELAY, LOW);

    // IO12: high at reset can force 1.8V flash timing and stop the board booting.
    digitalWrite(PIN_COIN_EN, LOW);

    // IO2: high at reset picks an unintended boot mode.
    digitalWrite(PIN_WDT_DONE, LOW);

    // IO15: released to an input rather than driven, so the reset straps the same
    // way a real power-on does. R30 + D7 share this node with the internal 45K
    // boot pull-up, so leaving it driven would make a software reset behave
    // differently from a power cycle — a difference that shows up as the ROM boot
    // log appearing or vanishing, and is confusing to debug.
    pinMode(PIN_LED_B, INPUT);
}

// =============================================================================
// Relay
// =============================================================================
void relay_on()  { digitalWrite(PIN_RELAY, HIGH); }
void relay_off() { digitalWrite(PIN_RELAY, LOW);  }

// =============================================================================
// Coin slot enable (IO12, active-HIGH)
// =============================================================================
static bool s_coin_slot_on = true;

void coin_slot_enable()  { s_coin_slot_on = true;  digitalWrite(PIN_COIN_EN, HIGH); }
void coin_slot_disable() { s_coin_slot_on = false; digitalWrite(PIN_COIN_EN, LOW);  }
bool coin_slot_enabled() { return s_coin_slot_on; }

// =============================================================================
// LEDs
// =============================================================================
void leds_set(bool on) {
    // One LED where the STM32 had two (PA2 + PA3 driven in lockstep).
    digitalWrite(PIN_LED_B, on ? HIGH : LOW);
}

// =============================================================================
// Buzzer — internal helper + named pattern implementations
//
// The STM32 used tone()/noTone() on a hardware timer. ESP32 Arduino has no
// tone(), so this drives LEDC directly. Driving a square wave rather than the
// gate DC is also the safer choice for unknown hardware: BZ1 may be a passive
// sounder, which makes no noise at all on a DC gate. If it turns out to be an
// active buzzer, a square wave still sounds it (just chopped) — whereas the
// reverse assumption would ship a silent machine.
// =============================================================================
static void buzzer_tone_start(uint32_t freq_hz) {
#if S1_LEDC_V3
    ledcAttach(PIN_BUZZER, freq_hz, 10);
    ledcWrite(PIN_BUZZER, 512);                 // 50% duty
#else
    ledcSetup(S1_BUZZ_CH, freq_hz, 10);
    ledcAttachPin(PIN_BUZZER, S1_BUZZ_CH);
    ledcWrite(S1_BUZZ_CH, 512);
#endif
}

static void buzzer_tone_stop() {
#if S1_LEDC_V3
    ledcDetach(PIN_BUZZER);
#else
    ledcDetachPin(PIN_BUZZER);
#endif
    // Detaching leaves the pad in an unspecified state, so re-assert the gate
    // low explicitly — otherwise the buzzer can be left sounding.
    out_safe_low(PIN_BUZZER);
}

static void beep(uint16_t freq_hz, uint8_t count, uint16_t on_ms, uint16_t off_ms) {
    for (uint8_t i = 0; i < count; ++i) {
        buzzer_tone_start(freq_hz);
        // vTaskDelay, not delay(): this runs in the app/session task and must
        // yield. Note what changed from the STM32 here — there, each beep also
        // kicked the watchdog, because a multi-beep pattern could otherwise
        // approach the IWDG window. The external TPL5010 window is ~100 ms, far
        // shorter than a single beep, so kicking it from this loop could never
        // have worked; the wdt task covers it instead. See src/wdt.h.
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        buzzer_tone_stop();
        if (i < count - 1) vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void buzzer_beep_session_start(const AppConfig* cfg) {
    // Low double-beep — "ready, go"
    DBG("[periph] start beep: ");
    DBG(cfg->beep_start_count);
    DBG("x @ ");
    DBG(cfg->beep_start_freq_hz);
    DBGLN(" Hz");
    beep(cfg->beep_start_freq_hz,
         cfg->beep_start_count,
         cfg->beep_start_on_ms,
         cfg->beep_off_ms);
}

void buzzer_beep_session_end(const AppConfig* cfg) {
    // High triple-beep — "done done done"
    DBG("[periph] end beep: ");
    DBG(cfg->beep_end_count);
    DBG("x @ ");
    DBG(cfg->beep_end_freq_hz);
    DBGLN(" Hz");
    beep(cfg->beep_end_freq_hz,
         cfg->beep_end_count,
         cfg->beep_end_on_ms,
         cfg->beep_off_ms);
}

void buzzer_beep_enter_config() {
    // TODO: short double-beep for config mode entry (stub on the STM32 too)
}

void buzzer_beep_exit_config() {
    // TODO: single long beep for config mode exit (stub on the STM32 too)
}

void buzzer_beep_denied() {
    // Single low buzz — distinct from the start (1000 Hz x2) and end (2500 Hz x3) tones
    DBGLN("[periph] denied beep");
    beep(300, 1, 400, 0);
}

// =============================================================================
// Coin input — FreeRTOS-task polled counter with debounce
//
// Kept as a polled task rather than moved to the interrupt the S1 bring-up
// harness used. Polling is what the STM32 did, the 5 ms cadence is far finer
// than a coin pulse, and this is the one input that represents money — matching
// the proven billing path matters more here than shaving latency.
// =============================================================================

static SemaphoreHandle_t s_coin_mutex = nullptr;
static volatile uint32_t s_coin_count = 0;
static volatile uint32_t s_coin_value_cents      = 0;
static volatile uint32_t s_price_per_credit_cents = 0;
static volatile bool     s_coin_active_high      = false;   // see AppConfig::coin_active_high

void coin_counter_init() {
    s_coin_mutex = xSemaphoreCreateMutex();
}

void coin_counter_set_polarity(bool active_high) {
    s_coin_active_high = active_high;
}

bool coin_counter_polarity() { return s_coin_active_high; }

bool coin_raw_level() { return digitalRead(PIN_COIN_IN) == HIGH; }

void coin_counter_task_run(void* arg) {
    (void)arg;

    // Debounce state machine: detect a debounced idle->active edge on PIN_COIN_IN.
    // Poll period: 5 ms | debounce threshold: 3 consecutive active samples = 15 ms.
    // Identical to the STM32 version except that "active" is resolved against
    // the configured polarity instead of being hard-coded to HIGH.
    enum CoinState { COIN_IDLE, COIN_DEBOUNCE, COIN_ACTIVE_WAIT };
    CoinState state       = COIN_IDLE;
    uint8_t   db_samples  = 0;
    uint16_t  stuck_ticks = 0;

    // 5 ms per poll, so 400 ticks is 2 s of continuous assertion. Generously
    // beyond any real coin pulse, and short enough that a technician sees the
    // fault while still standing at the machine.
    static const uint16_t COIN_STUCK_TICKS = 400;
    const uint8_t DB_THRESHOLD = 3;

    Serial.println("[coin] counter task running");

    for (;;) {
        const bool active = (digitalRead(PIN_COIN_IN) == HIGH) == s_coin_active_high;

        switch (state) {
            case COIN_IDLE:
                if (active) { db_samples = 1; state = COIN_DEBOUNCE; }
                break;

            case COIN_DEBOUNCE:
                if (active) {
                    if (++db_samples >= DB_THRESHOLD) {
                        xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
                        ++s_coin_count;
                        s_coin_value_cents += s_price_per_credit_cents;
                        const uint32_t total = s_coin_count;
                        const uint32_t billed = s_price_per_credit_cents;
                        xSemaphoreGive(s_coin_mutex);

                        // ESP32-only: operator earnings totals for the BLE Live
                        // Counters characteristic. No STM32 counterpart — see
                        // docs/PORTING_FROM_STM32.md 2.5a.
                        //
                        // Deliberately HERE, at acceptance, and not at session
                        // end: the app's Diagnostics coin-path wizard proves a
                        // coin was seen by watching lifetimeAmount move, so a
                        // per-session increment would report a dead coin path on
                        // a working board. Called outside the coin mutex --
                        // counters keeps its own, and nesting the two would
                        // invent a lock-ordering rule for no reason.
                        counters_record_coin(billed);
                        DBG("[coin] pulse detected, credits=");
                        DBGLN(total);
                        state = COIN_ACTIVE_WAIT;
                    }
                } else {
                    db_samples = 0;
                    state = COIN_IDLE;
                }
                break;

            case COIN_ACTIVE_WAIT:
                if (!active) {
                    stuck_ticks = 0;
                    state = COIN_IDLE;
                } else if (stuck_ticks < COIN_STUCK_TICKS) {
                    // ESP32-only: stuck-line detection for BLE diagnostics
                    // error 0x01. No STM32 counterpart — see
                    // docs/PORTING_FROM_STM32.md 2.5a.
                    //
                    // A real acceptor pulse is tens of milliseconds. A line
                    // held asserted for seconds is not a coin: it is a shorted
                    // harness, a failed opto, or — the case worth catching —
                    // the WRONG POLARITY, which parks the input "active"
                    // forever. That last one otherwise presents as a machine
                    // that counted one phantom credit at power-on and then
                    // never counts again, with nothing to explain it.
                    if (++stuck_ticks >= COIN_STUCK_TICKS) {
                        diag_raise(DIAG_ERR_COIN_LINE);
                        DBGLN("[coin] line stuck asserted - raised diag 0x01");
                    }
                }
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool coin_detected() {
    xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
    const bool v = (s_coin_count > 0);
    xSemaphoreGive(s_coin_mutex);
    return v;
}

uint32_t coin_get_count() {
    xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
    const uint32_t v = s_coin_count;
    xSemaphoreGive(s_coin_mutex);
    return v;
}

void coin_consume_value_cents(uint32_t cents) {
    xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
    s_coin_value_cents = (s_coin_value_cents > cents) ? (s_coin_value_cents - cents) : 0;
    xSemaphoreGive(s_coin_mutex);
}

void coin_reset() {
    xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
    s_coin_count = 0;
    s_coin_value_cents = 0;
    xSemaphoreGive(s_coin_mutex);
}

void coin_counter_set_price_per_credit(uint32_t price_cents) {
    xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
    s_price_per_credit_cents = price_cents;
    xSemaphoreGive(s_coin_mutex);
}

uint32_t coin_get_value_cents() {
    xSemaphoreTake(s_coin_mutex, portMAX_DELAY);
    const uint32_t v = s_coin_value_cents;
    xSemaphoreGive(s_coin_mutex);
    return v;
}

// =============================================================================
// Buttons
// =============================================================================

// -----------------------------------------------------------------------------
// Simple debounce helper — fires on initial press confirmation, active-LOW,
// then auto-repeats while held so B1/B2 can scrub a config value quickly
// instead of needing one tap per step.
// States: 0=IDLE, 1=DEBOUNCE, 2=HELD, 3=RELEASE_DB
//
// Hold timing assumes this is polled once per ~20 ms loop iteration (true for
// all callers below, driven by config_menu_run() and the IDLE loop):
//   REPEAT_DELAY_TICKS  — held time before auto-repeat kicks in (500 ms)
//   REPEAT_SLOW_TICKS   — repeat cadence right after the delay (160 ms)
//   REPEAT_ACCEL_TICKS  — additional held time (past the delay) before the
//                         cadence speeds up, for fast add/subtract (1500 ms)
//   REPEAT_FAST_TICKS   — repeat cadence once accelerated (40 ms)
// -----------------------------------------------------------------------------
#define BTN_REPEAT_DELAY_TICKS   25
#define BTN_REPEAT_SLOW_TICKS     8
#define BTN_REPEAT_ACCEL_TICKS   75
#define BTN_REPEAT_FAST_TICKS     2

struct BtnSimple { uint8_t state; uint8_t db; uint16_t hold_ticks; uint16_t repeat_ticks; };

// `repeat` gates the HELD-state auto-repeat: pass true for B1/B2 (config-menu
// scrubbing) and false for the start button, which must fire exactly once per
// press regardless of how long it is held. See the note on
// start_button_pressed() in periph.h — this parameter is the fix for a real
// relay-chatter bug, not a stylistic knob.
static bool btn_simple_poll(uint8_t pin, BtnSimple* b, bool repeat) {
    const bool low = (digitalRead(pin) == LOW);
    bool fired = false;
    switch (b->state) {
        case 0:  if (low) { b->db = 1; b->state = 1; }  break;
        case 1:
            if (low) {
                if (++b->db >= 3) {
                    fired         = true;
                    b->state      = 2;
                    b->hold_ticks = 0;
                    b->repeat_ticks = 0;
                }
            } else { b->db = 0; b->state = 0; }
            break;
        case 2:
            if (low) {
                if (repeat) {
                    ++b->hold_ticks;
                    if (b->hold_ticks >= BTN_REPEAT_DELAY_TICKS) {
                        const uint16_t cadence =
                            (b->hold_ticks >= BTN_REPEAT_DELAY_TICKS + BTN_REPEAT_ACCEL_TICKS)
                                ? BTN_REPEAT_FAST_TICKS : BTN_REPEAT_SLOW_TICKS;
                        if (++b->repeat_ticks >= cadence) {
                            b->repeat_ticks = 0;
                            fired = true;
                        }
                    }
                }
            } else { b->db = 0; b->state = 3; }
            break;
        case 3:
            if (!low) { if (++b->db >= 3) b->state = 0; }
            else       { b->db = 0; b->state = 2; }
            break;
    }
    return fired;
}

static BtnSimple s_btn1 = {0, 0, 0, 0};
static BtnSimple s_btn2 = {0, 0, 0, 0};

// A press is only a value edit when the OTHER button is not also down —
// otherwise it is part of the BTN1+BTN2 combo and must not touch the value.
//
// This matters because of a difference from the STM32 gesture set. There, the
// combo meant CANCEL: any value scrubbing it caused along the way was discarded
// anyway, so it was harmless and the original code ignored it. Here the combo
// means SAVE, so anything it scrubs gets committed. Worse, it is not even
// symmetric — on the boolean pages (Per Credit, Accumulation, Sensor Stop,
// Sensor Polarity) BTN1 writes false and BTN2 writes true from the same
// iteration, and BTN2 is evaluated second, so simply holding both to save would
// silently force those settings ON.
//
// The state machine is still polled either way, so debounce and hold timing stay
// coherent — only the `fired` result is suppressed. Releasing one button while
// keeping the other held therefore resumes scrubbing with that button, which is
// what an operator would expect.
static bool other_btn_down(uint8_t other_pin) {
    return digitalRead(other_pin) == LOW;
}

bool btn_b1_pressed() {
    const bool fired = btn_simple_poll(PIN_BTN1, &s_btn1, true);
    return fired && !other_btn_down(PIN_BTN2);
}

bool btn_b2_pressed() {
    const bool fired = btn_simple_poll(PIN_BTN2, &s_btn2, true);
    return fired && !other_btn_down(PIN_BTN1);
}

// -----------------------------------------------------------------------------
// PIN_USER_BTN (START) — shared state machine for press-confirm, short and long
//
//   PRESS  — debounce confirmed        → fires immediately  (start_button_pressed)
//   SHORT  — released before LONG      → fires on release   (start_button_short_released)
//   LONG   — held past LONG_TICKS      → fires while held   (start_button_long_pressed)
//
// This is the STM32 BTN3 state machine shape, moved onto the start button
// because the S1 has no BTN3 (see the gesture map in pins.h). The PRESS flag is
// the STM32 start_button_pressed() semantics, preserved exactly: it fires on
// confirmation, not release, and never auto-repeats.
//
// One machine serving all three is what guarantees a single press cannot mean
// two things — a long press fires LONG and never SHORT, so cancelling the config
// menu can never also turn a page on the way out.
//
// btn_update() is tick-guarded so it advances at most once per FreeRTOS tick
// even when several consumer functions are called in the same loop iteration.
// -----------------------------------------------------------------------------
#define BTN_DB_THRESH   3      // x 20 ms = 60 ms debounce
#define BTN_LONG_TICKS  75     // x 20 ms = 1500 ms long-press threshold

enum StartBtnState { SB_IDLE, SB_DEBOUNCE, SB_HELD, SB_LONG_HELD, SB_RELEASE_DB };

static StartBtnState s_sb_state = SB_IDLE;
static uint8_t       s_sb_db    = 0;
static uint16_t      s_sb_hold  = 0;
static bool          s_sb_press = false;
static bool          s_sb_short = false;
static bool          s_sb_long  = false;

static void start_btn_update() {
    static TickType_t s_last = 0;
    const TickType_t now = xTaskGetTickCount();
    if (now == s_last) return;
    s_last = now;

    const bool low = (digitalRead(PIN_USER_BTN) == LOW);

    switch (s_sb_state) {
        case SB_IDLE:
            if (low) { s_sb_db = 1; s_sb_state = SB_DEBOUNCE; }
            break;
        case SB_DEBOUNCE:
            if (low) {
                if (++s_sb_db >= BTN_DB_THRESH) {
                    s_sb_press = true;      // press-confirm — the IDLE/session meaning
                    s_sb_hold  = 0;
                    s_sb_state = SB_HELD;
                }
            } else { s_sb_db = 0; s_sb_state = SB_IDLE; }
            break;
        case SB_HELD:
            if (low) {
                if (++s_sb_hold >= BTN_LONG_TICKS) {
                    s_sb_long  = true;
                    s_sb_state = SB_LONG_HELD;
                }
            } else {
                s_sb_short = true;          // released before the long threshold
                s_sb_db    = 0;
                s_sb_state = SB_RELEASE_DB;
            }
            break;
        case SB_LONG_HELD:
            if (!low) s_sb_state = SB_IDLE;
            break;
        case SB_RELEASE_DB:
            if (!low) {
                if (++s_sb_db >= BTN_DB_THRESH) s_sb_state = SB_IDLE;
            } else { s_sb_db = 0; s_sb_state = SB_HELD; }
            break;
    }
}

bool start_button_pressed() {
    start_btn_update();
    if (s_sb_press) { s_sb_press = false; return true; }
    return false;
}

bool start_button_short_released() {
    start_btn_update();
    if (s_sb_short) { s_sb_short = false; return true; }
    return false;
}

bool start_button_long_pressed() {
    start_btn_update();
    if (s_sb_long) { s_sb_long = false; return true; }
    return false;
}

void start_button_flush() {
    s_sb_press = false;
    s_sb_short = false;
    s_sb_long  = false;
}

// -----------------------------------------------------------------------------
// PIN_BTN1 + PIN_BTN2 held together — the S1 combo gesture (enter config from
// IDLE, save from inside the menu). Same debounce/hold thresholds as the start
// button, but requires both pins LOW at once; releasing either pin before the
// long-press threshold aborts back to idle (no short-press variant needed here).
//
// The LONG_HELD state is what makes this safe to use for two different meanings
// back to back: after firing, it waits for BOTH buttons to be released before it
// can arm again. So the hold that enters the config menu is still in LONG_HELD
// when the menu starts polling, and cannot instantly re-fire as a save.
// -----------------------------------------------------------------------------
enum Btn12State { B12_IDLE, B12_DEBOUNCE, B12_HELD, B12_LONG_HELD };

static Btn12State s_btn12_state = B12_IDLE;
static uint8_t    s_btn12_db    = 0;
static uint16_t   s_btn12_hold  = 0;
static bool       s_btn12_long  = false;

static void btn12_update() {
    static TickType_t s_last = 0;
    const TickType_t now = xTaskGetTickCount();
    if (now == s_last) return;
    s_last = now;

    const bool both_low = (digitalRead(PIN_BTN1) == LOW) && (digitalRead(PIN_BTN2) == LOW);

    switch (s_btn12_state) {
        case B12_IDLE:
            if (both_low) { s_btn12_db = 1; s_btn12_state = B12_DEBOUNCE; }
            break;
        case B12_DEBOUNCE:
            if (both_low) {
                if (++s_btn12_db >= BTN_DB_THRESH) {
                    s_btn12_hold  = 0;
                    s_btn12_state = B12_HELD;
                }
            } else { s_btn12_db = 0; s_btn12_state = B12_IDLE; }
            break;
        case B12_HELD:
            if (both_low) {
                if (++s_btn12_hold >= BTN_LONG_TICKS) {
                    s_btn12_long  = true;
                    s_btn12_state = B12_LONG_HELD;
                }
            } else {
                s_btn12_state = B12_IDLE;
            }
            break;
        case B12_LONG_HELD:
            if (!both_low) s_btn12_state = B12_IDLE;
            break;
    }
}

bool btn_b1_b2_long_pressed() {
    btn12_update();
    if (s_btn12_long) { s_btn12_long = false; return true; }
    return false;
}

// =============================================================================
// Sensor input (Sensor Stop)
// =============================================================================
bool sensor_read_active(bool active_high) {
    const bool high = (digitalRead(PIN_SENSOR_IN) == HIGH);
    return active_high ? high : !high;
}

// =============================================================================
// Board role strap
// =============================================================================
bool strap_role_high() { return digitalRead(PIN_I2C_CONF) == HIGH; }
