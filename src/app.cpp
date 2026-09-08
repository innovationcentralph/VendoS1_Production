#include "app.h"
#include "config.h"
#include "session.h"
#include "periph.h"
#include "display.h"
#include "debug_log.h"
#include "wdt.h"
#include <Arduino.h>

// =============================================================================
// config_menu_run — ten-page config editor
//
// Pages (START tap cycles between them):
//   Page 0 — Operation Mode     : BTN1 prev | BTN2 next (implemented modes only)
//   Page 1 — Inactivity Timer   : BTN1 -10  | BTN2 +10  s (0-3600; OP_PRESS_TO_START or OP_PAUSE_RESUME)
//   Page 2 — Per Credit         : BTN1 OFF  | BTN2 ON   (OP_PRESS_TO_START or OP_PAUSE_RESUME)
//   Page 3 — Coin per Credit    : BTN1 -1   | BTN2 +1   (1-99 credits)
//   Page 4 — Relay Time         : BTN1 -1 s | BTN2 +1 s (1-3600 s)
//   Page 5 — Accumulation       : BTN1 OFF  | BTN2 ON   (composable with any mode above)
//   Page 6 — Sensor Slot        : BTN1 OFF  | BTN2 ON   (composable with any mode above)
//   Page 7 — Sensor Slot Config : BTN1 LOW  | BTN2 HIGH (active level / polarity)
//   Page 8 — Sensor Wait Time   : BTN1 -10  | BTN2 +10  ms (0-5000; 0 = immediate)
//   Page 9 — Price Per Credit   : BTN1 -P1  | BTN2 +P1  (peso value of one coin credit)
//
// Pages 1-2 deliberately follow Operation Mode (page 0). Page 1 (Inactivity
// Timer) is shared: it drives the initial auto-start safety net for BOTH
// OP_PRESS_TO_START and OP_PAUSE_RESUME (see IDLE in app_task_run), so it
// stays visible for either — only hidden for other modes (Auto Start already
// starts on its own and has no use for this value). Page 2 (Per Credit) means
// anything for that same pair of modes and is hidden otherwise.
//
// There is no separate coin-slot page: whether the coin slot stays live
// during an active session is derived directly from Accumulation (page 5) —
// ON keeps it enabled (accumulation needs live coin input mid-session), OFF
// disables it for the whole session (see session_run()).
//
// Pages 7-8 only make sense when Sensor Slot (page 6) is enabled — same
// hide-while-irrelevant treatment.
//
// -----------------------------------------------------------------------------
// GESTURES — this is where the S1 diverges from the STM32
// -----------------------------------------------------------------------------
// The STM32 had three config buttons; this board has two (SW2/SW3). BTN3 duties
// moved onto the START button and the BTN1+BTN2 combo. Full rationale and the
// side-by-side mapping are in include/pins.h; the short version:
//
//   START tap                    — navigate to next page (wraps)   [was BTN3 short]
//   START long press (~1.5 s)    — CANCEL (no change), return false [was BTN1+BTN2 long]
//   BTN1+BTN2 long press (~1.5 s)— SAVE all pages to *cfg, return true [was BTN3 long]
//
// The checks below are ordered save, then cancel, then page — deliberately the
// same precedence the STM32 used, so that if two gestures somehow resolve in
// one iteration the committing one wins over the discarding one, and neither
// loses to a page turn.
// =============================================================================

// Working copy of every field editable across the menu pages — bundled into
// one struct once the page count made a loose per-field parameter list
// unwieldy (and error-prone to keep in sync across 4 call sites).
struct MenuState {
    uint32_t relay_ms;
    uint32_t credits_req;
    uint8_t  opmode_idx;    // index into kImplementedOpModes
    uint8_t  opmode;        // actual OperationMode value = kImplementedOpModes[opmode_idx]
    bool     per_credit;
    uint16_t inactivity_timeout_s;
    bool     accumulation_enabled;
    bool     sensor_enable;
    bool     sensor_active_hi;
    uint16_t sensor_wait_ms;    // 0 = immediate, >0 = required continuous-active ms
    uint32_t price_per_credit_cents;   // peso value of one coin credit, in centavos
};

static void config_menu_show_page(uint8_t page, const MenuState& s) {
    switch (page) {
        case 0: display_show_config_opmode(s.opmode);                          break;
        case 1: display_show_config_inactivity_timeout(s.inactivity_timeout_s); break;
        case 2: display_show_config_per_credit(s.per_credit);                  break;
        case 3: display_show_config_credits(s.credits_req);                    break;
        case 4: display_show_config(s.relay_ms);                              break;
        case 5: display_show_config_accumulation(s.accumulation_enabled);      break;
        case 6: display_show_config_sensor_enable(s.sensor_enable);           break;
        case 7: display_show_config_sensor_polarity(s.sensor_active_hi);      break;
        case 8: display_show_config_sensor_wait(s.sensor_wait_ms);            break;
        default: display_show_config_price(s.price_per_credit_cents);        break;
    }
}

// Find mode position in kImplementedOpModes; falls back to index 0
// (OP_PRESS_TO_START) if the stored value is not one of the implemented modes
// (e.g. an unimplemented value written before this mode existed).
static uint8_t opmode_index_of(uint8_t mode) {
    for (uint8_t i = 0; i < NUM_OPERATION_MODES_IMPLEMENTED; ++i) {
        if (kImplementedOpModes[i] == mode) return i;
    }
    return 0;
}

static bool config_menu_run(AppConfig* cfg) {
    const uint32_t STEP           = 1000;
    const uint32_t MIN_MS         = 1000;
    const uint32_t MAX_MS         = 3600000;
    const uint32_t MIN_CREDITS    = 1;
    const uint32_t MAX_CREDITS    = 99;
    const uint16_t WAIT_STEP      = 10;
    const uint16_t MAX_WAIT       = 5000;
    const uint16_t TIMEOUT_STEP   = 10;
    const uint16_t MAX_TIMEOUT_S  = 3600;
    const uint32_t PRICE_STEP        = 100;    // P1.00 increments
    const uint32_t MAX_PRICE_CENTS   = 99999;  // P999.99 cap
    const uint8_t  NUM_PAGES      = 10;

    MenuState s;
    s.relay_ms             = cfg->relay_on_ms;
    s.credits_req          = cfg->coins_required;
    s.opmode_idx           = opmode_index_of(cfg->operation_mode);
    s.opmode               = kImplementedOpModes[s.opmode_idx];
    s.per_credit           = cfg->press_to_start_per_credit;
    s.inactivity_timeout_s = cfg->inactivity_timeout_s;
    s.accumulation_enabled = cfg->accumulation_enabled;
    s.sensor_enable        = cfg->sensor_stop_enabled;
    s.sensor_active_hi     = cfg->sensor_active_high;
    s.sensor_wait_ms       = cfg->sensor_wait_ms;
    s.price_per_credit_cents = cfg->price_per_credit_cents;
    uint8_t page = 0;   // 0=opmode,1=inactivity timer,2=per credit,3=coin per credit,
                        // 4=relay time,5=accumulation,6=sensor slot,
                        // 7=sensor slot config,8=sensor wait time,9=price per credit

    // Discard any START one-shot left over from IDLE. Without this, a START tap
    // that happened just before the operator got the BTN1+BTN2 combo in would
    // still be pending and would turn a page the instant the menu opened.
    start_button_flush();

    config_menu_show_page(page, s);
    DBG("[config] enter — relay_on_ms="); DBGLN(s.relay_ms);
    DBG("[config] coins_required="); DBGLN(s.credits_req);
    DBG("[config] operation_mode="); DBGLN(s.opmode);
    DBG("[config] press_to_start_per_credit="); DBGLN(s.per_credit ? "ON" : "OFF");
    DBG("[config] inactivity_timeout_s="); DBGLN(s.inactivity_timeout_s);
    DBG("[config] accumulation_enabled="); DBGLN(s.accumulation_enabled ? "ON" : "OFF");
    DBG("[config] sensor_stop_enabled="); DBGLN(s.sensor_enable ? "ON" : "OFF");
    DBG("[config] sensor_active_high="); DBGLN(s.sensor_active_hi ? "HIGH" : "LOW");
    DBG("[config] sensor_wait_ms="); DBGLN(s.sensor_wait_ms);
    DBG("[config] price_per_credit_cents="); DBGLN(s.price_per_credit_cents);

    TickType_t last_repost_tick = xTaskGetTickCount();

    for (;;) {
        switch (page) {
            case 0:
                // Page 0 — Operation Mode (cycles kImplementedOpModes, not raw values —
                // implemented modes are not contiguous, see config.h)
                if (btn_b1_pressed()) {
                    s.opmode_idx = (s.opmode_idx > 0) ? s.opmode_idx - 1 : (NUM_OPERATION_MODES_IMPLEMENTED - 1);
                    s.opmode = kImplementedOpModes[s.opmode_idx];
                    display_show_config_opmode(s.opmode);
                    DBG("[config] operation_mode="); DBGLN(s.opmode);
                }
                if (btn_b2_pressed()) {
                    s.opmode_idx = (s.opmode_idx + 1) % NUM_OPERATION_MODES_IMPLEMENTED;
                    s.opmode = kImplementedOpModes[s.opmode_idx];
                    display_show_config_opmode(s.opmode);
                    DBG("[config] operation_mode="); DBGLN(s.opmode);
                }
                break;
            case 1:
                // Page 1 — Inactivity Timeout. The stored value governs both
                // OP_PRESS_TO_START and OP_PAUSE_RESUME initial auto-start —
                // see IDLE in app_task_run. 0 = disabled (wait indefinitely).
                if (btn_b1_pressed()) {
                    s.inactivity_timeout_s = (s.inactivity_timeout_s > TIMEOUT_STEP)
                        ? s.inactivity_timeout_s - TIMEOUT_STEP : 0;
                    display_show_config_inactivity_timeout(s.inactivity_timeout_s);
                    DBG("[config] inactivity_timeout_s="); DBGLN(s.inactivity_timeout_s);
                }
                if (btn_b2_pressed()) {
                    s.inactivity_timeout_s = (s.inactivity_timeout_s < MAX_TIMEOUT_S - TIMEOUT_STEP)
                        ? s.inactivity_timeout_s + TIMEOUT_STEP : MAX_TIMEOUT_S;
                    display_show_config_inactivity_timeout(s.inactivity_timeout_s);
                    DBG("[config] inactivity_timeout_s="); DBGLN(s.inactivity_timeout_s);
                }
                break;
            case 2:
                // Page 2 — Per Credit Mode (OP_PRESS_TO_START or OP_PAUSE_RESUME):
                // dispense one credit block per button press instead of all at once
                if (btn_b1_pressed()) {
                    s.per_credit = false;
                    display_show_config_per_credit(s.per_credit);
                    DBGLN("[config] press_to_start_per_credit=OFF");
                }
                if (btn_b2_pressed()) {
                    s.per_credit = true;
                    display_show_config_per_credit(s.per_credit);
                    DBGLN("[config] press_to_start_per_credit=ON");
                }
                break;
            case 3:
                // Page 3 — Coin per Credit (coins_required — minimum/block size)
                if (btn_b1_pressed()) {
                    s.credits_req = (s.credits_req > MIN_CREDITS) ? s.credits_req - 1 : MIN_CREDITS;
                    display_show_config_credits(s.credits_req);
                    DBG("[config] coins_required="); DBGLN(s.credits_req);
                }
                if (btn_b2_pressed()) {
                    s.credits_req = (s.credits_req < MAX_CREDITS) ? s.credits_req + 1 : MAX_CREDITS;
                    display_show_config_credits(s.credits_req);
                    DBG("[config] coins_required="); DBGLN(s.credits_req);
                }
                break;
            case 4:
                // Page 4 — Relay ON Time
                if (btn_b1_pressed()) {
                    s.relay_ms = (s.relay_ms > MIN_MS) ? s.relay_ms - STEP : MIN_MS;
                    display_show_config(s.relay_ms);
                    DBG("[config] relay_on_ms="); DBGLN(s.relay_ms);
                }
                if (btn_b2_pressed()) {
                    s.relay_ms = (s.relay_ms < MAX_MS) ? s.relay_ms + STEP : MAX_MS;
                    display_show_config(s.relay_ms);
                    DBG("[config] relay_on_ms="); DBGLN(s.relay_ms);
                }
                break;
            case 5:
                // Page 5 — Accumulation enable (composable with any operation_mode).
                // Also decides whether the coin slot stays live during an active
                // session: ON keeps it enabled (accumulation needs live coin
                // input), OFF disables it for the whole session — see session_run().
                if (btn_b1_pressed()) {
                    s.accumulation_enabled = false;
                    display_show_config_accumulation(s.accumulation_enabled);
                    DBGLN("[config] accumulation_enabled=OFF");
                }
                if (btn_b2_pressed()) {
                    s.accumulation_enabled = true;
                    display_show_config_accumulation(s.accumulation_enabled);
                    DBGLN("[config] accumulation_enabled=ON");
                }
                break;
            case 6:
                // Page 6 — Sensor Slot enable (composable with any operation_mode)
                if (btn_b1_pressed()) {
                    s.sensor_enable = false;
                    display_show_config_sensor_enable(s.sensor_enable);
                    DBGLN("[config] sensor_stop_enabled=OFF");
                }
                if (btn_b2_pressed()) {
                    s.sensor_enable = true;
                    display_show_config_sensor_enable(s.sensor_enable);
                    DBGLN("[config] sensor_stop_enabled=ON");
                }
                break;
            case 7:
                // Page 7 — Sensor Slot Config (polarity / active level)
                if (btn_b1_pressed()) {
                    s.sensor_active_hi = false;
                    display_show_config_sensor_polarity(s.sensor_active_hi);
                    DBGLN("[config] sensor_active_high=LOW");
                }
                if (btn_b2_pressed()) {
                    s.sensor_active_hi = true;
                    display_show_config_sensor_polarity(s.sensor_active_hi);
                    DBGLN("[config] sensor_active_high=HIGH");
                }
                break;
            case 8:
                // Page 8 — Sensor wait time (0 = immediate, >0 = required continuous-active ms)
                if (btn_b1_pressed()) {
                    s.sensor_wait_ms = (s.sensor_wait_ms > WAIT_STEP) ? s.sensor_wait_ms - WAIT_STEP : 0;
                    display_show_config_sensor_wait(s.sensor_wait_ms);
                    DBG("[config] sensor_wait_ms="); DBGLN(s.sensor_wait_ms);
                }
                if (btn_b2_pressed()) {
                    s.sensor_wait_ms = (s.sensor_wait_ms < MAX_WAIT - WAIT_STEP)
                        ? s.sensor_wait_ms + WAIT_STEP : MAX_WAIT;
                    display_show_config_sensor_wait(s.sensor_wait_ms);
                    DBG("[config] sensor_wait_ms="); DBGLN(s.sensor_wait_ms);
                }
                break;
            default:
                // Page 9 — Price Per Credit (peso value of one raw coin/credit
                // pulse) — feeds the LCD idle prompt and independent coin-value
                // total; unrelated to coins_required (the block-size threshold).
                if (btn_b1_pressed()) {
                    s.price_per_credit_cents = (s.price_per_credit_cents > PRICE_STEP)
                        ? s.price_per_credit_cents - PRICE_STEP : 0;
                    display_show_config_price(s.price_per_credit_cents);
                    DBG("[config] price_per_credit_cents="); DBGLN(s.price_per_credit_cents);
                }
                if (btn_b2_pressed()) {
                    s.price_per_credit_cents = (s.price_per_credit_cents < MAX_PRICE_CENTS - PRICE_STEP)
                        ? s.price_per_credit_cents + PRICE_STEP : MAX_PRICE_CENTS;
                    display_show_config_price(s.price_per_credit_cents);
                    DBG("[config] price_per_credit_cents="); DBGLN(s.price_per_credit_cents);
                }
                break;
        }

        // BTN1+BTN2 long press -> SAVE all settings.
        // (STM32 used BTN3 long press. The combo cannot double-fire from the
        // hold that opened this menu — see btn_b1_b2_long_pressed() in periph.h.)
        if (btn_b1_b2_long_pressed()) {
            cfg->relay_on_ms              = s.relay_ms;
            cfg->coins_required           = s.credits_req;
            cfg->operation_mode           = s.opmode;
            cfg->press_to_start_per_credit = s.per_credit;
            cfg->inactivity_timeout_s     = s.inactivity_timeout_s;
            cfg->accumulation_enabled     = s.accumulation_enabled;
            cfg->sensor_stop_enabled      = s.sensor_enable;
            cfg->sensor_active_high       = s.sensor_active_hi;
            cfg->sensor_wait_ms           = s.sensor_wait_ms;
            cfg->price_per_credit_cents  = s.price_per_credit_cents;
            DBG("[config] SAVED relay_on_ms="); DBGLN(s.relay_ms);
            DBG("[config] SAVED coins_required="); DBGLN(s.credits_req);
            DBG("[config] SAVED operation_mode="); DBGLN(s.opmode);
            DBG("[config] SAVED press_to_start_per_credit="); DBGLN(s.per_credit ? "ON" : "OFF");
            DBG("[config] SAVED inactivity_timeout_s="); DBGLN(s.inactivity_timeout_s);
            DBG("[config] SAVED accumulation_enabled="); DBGLN(s.accumulation_enabled ? "ON" : "OFF");
            DBG("[config] SAVED sensor_stop_enabled="); DBGLN(s.sensor_enable ? "ON" : "OFF");
            DBG("[config] SAVED sensor_active_high="); DBGLN(s.sensor_active_hi ? "HIGH" : "LOW");
            DBG("[config] SAVED sensor_wait_ms="); DBGLN(s.sensor_wait_ms);
            DBG("[config] SAVED price_per_credit_cents="); DBGLN(s.price_per_credit_cents);
            start_button_flush();   // the exit gesture must not start a session
            return true;
        }

        // START long press -> CANCEL without saving.
        // (STM32 used BTN1+BTN2 long press, which is now the save gesture.)
        // Checked before the short-press page turn, and the shared state machine
        // guarantees a long press never also raises the short flag — so cancelling
        // cannot also turn a page on the way out.
        if (start_button_long_pressed()) {
            DBGLN("[config] CANCELLED");
            start_button_flush();
            return false;
        }

        // START tap — navigates to the next page. (STM32 used BTN3 short press.)
        if (start_button_short_released()) {
            page = (page + 1) % NUM_PAGES;
            // Page 1 (Inactivity Timer) matters for OP_PRESS_TO_START AND
            // OP_PAUSE_RESUME (both use it as the initial auto-start safety
            // net) — skipped for any other mode (e.g. Auto Start, which
            // already starts on its own and has no use for this value).
            // Page 2 (Per Credit) is relevant to that same pair of modes.
            const bool timeout_relevant = (s.opmode == OP_PRESS_TO_START || s.opmode == OP_PAUSE_RESUME);
            if (!timeout_relevant && page == 1) page = 2;
            if (!timeout_relevant && page == 2) page = 3;
            // Pages 7-8 (sensor slot config/wait time) only matter when Sensor
            // Slot is actually enabled — skip forward to page 9 (Price Per
            // Credit, always shown) instead of showing settings for a feature
            // that is currently off. Page 9 itself must stay reachable regardless.
            if (!s.sensor_enable && page >= 7 && page <= 8) page = 9;
            config_menu_show_page(page, s);
            DBG("[config] page -> "); DBGLN(page);
        }

        // Repost the current page once per second even with no button activity —
        // keeps the display task 5 s no-message heartbeat from force-rendering
        // the idle/home screen over this config page (see display_task_run()).
        if ((xTaskGetTickCount() - last_repost_tick) >= pdMS_TO_TICKS(1000)) {
            last_repost_tick = xTaskGetTickCount();
            config_menu_show_page(page, s);
        }

        watchdog_feed(WDT_SRC_MENU);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// =============================================================================
// app_task_run — Main Application State Machine
//
// Happy-path flow:
//   INIT -> IDLE --[START tap, coins inserted >= coins_required]--> COIN_VALIDATE
//                                                                          |
//                                                snapshot billed credits, consume just that many
//                                                    total_ms = credits x relay_on_ms
//                                                                          |
//                                                              SESSION_ACTIVE (session.cpp)
//                                                                          |
//                                                               SESSION_END -> IDLE
//
// Config mode flow (from IDLE):
//   BTN1+BTN2 long press -> CONFIG_MODE -> config_menu_run() -> IDLE
//   (STM32 used a BTN3 long press; see the gesture map in include/pins.h)
//
// Billing model (money-first — coins_required is a PESO PRICE, not a pulse
// count; see AppConfig::coins_required in config.h):
//   Credit           = floor(coin_get_value_cents() / (coins_required * 100))
//   Time to Dispense = Credit * relay_on_ms
//   Coins Inserted   = raw pulses * price_per_credit_cents (periph.cpp)
// Leftover cents that do not reach a whole credit are never discarded — they
// stay banked in coin_get_value_cents() for the next credit/session.
//
// PIN_USER_LED:
//   ON  when coin_get_value_cents() >= coins_required * 100  ("ready — press button"),
//       or, under per-credit gating, while remaining_credits > 0 in
//       APP_STATE_WAIT_NEXT_CREDIT (already-paid-for credit waiting on the
//       next press — same "ready" signal, just not tied to fresh coin_get_value_cents())
//   OFF otherwise or during session
// =============================================================================
void app_task_run(void* arg) {
    (void)arg;

    AppConfig cfg;
    AppState  state             = APP_STATE_INIT;
    uint32_t  session_credits   = 0;       // snapshot taken in COIN_VALIDATE
    uint32_t  remaining_credits = 0;       // (OP_PRESS_TO_START or OP_PAUSE_RESUME) +
                                            // press_to_start_per_credit only: whole blocks
                                            // still to dispense one at a time
    uint32_t  last_idle_money_cents = UINT32_MAX; // forces redraw on first IDLE entry
    TickType_t ready_since_tick        = 0; // IDLE: tick credits became ready (inactivity_timeout_s)
    TickType_t last_money_change_tick  = 0; // IDLE: tick money_cents last changed (OP_AUTO_START settle window)
    TickType_t wait_next_credit_start  = 0; // WAIT_NEXT_CREDIT: tick this wait began
    TickType_t wait_next_credit_repost = 0; // WAIT_NEXT_CREDIT: last display repost tick

    // OP_AUTO_START only: once ready, wait this long with no further coin
    // activity before actually firing the session. A coin acceptor emits
    // several pulses per physical coin (and a customer may drop a second coin
    // right behind the first) over tens-to-hundreds of ms — without this
    // grace window, Auto Start fires the instant the FIRST threshold-crossing
    // pulse lands, billing only part of that same insertion and losing the
    // rest to (or past) the already-started session instead of counting it
    // together. E.g. coins_required=5: two 5-peso coins dropped back-to-back
    // would bill only 1 credit instead of 2. 500 ms is comfortably longer
    // than any realistic acceptor inter-pulse/inter-coin gap for one deposit,
    // yet still reads as instant to the customer.
    const TickType_t AUTO_START_SETTLE_TICKS = pdMS_TO_TICKS(500);

    for (;;) {
        switch (state) {

        // ----------------------------------------------------------------
        // Power On
        //   -> Read Config from NVS
        //   -> Initialize Hardware State (Relay off, LEDs off, USER_LED off)
        // ----------------------------------------------------------------
        case APP_STATE_INIT:
            if (!config_load(&cfg)) {
                Serial.println("[app] using factory defaults");
            }
            periph_init();
            display_init(&cfg);
            display_show_boot();
            coin_counter_set_price_per_credit(cfg.price_per_credit_cents);
            coin_counter_set_polarity(cfg.coin_active_high);   // ESP32-only, see config.h
            display_set_idle_pricing(cfg.coins_required, cfg.relay_on_ms,
                                      (cfg.operation_mode == OP_PRESS_TO_START ||
                                       cfg.operation_mode == OP_PAUSE_RESUME) && cfg.press_to_start_per_credit);
            relay_off();
            leds_set(false);
            digitalWrite(PIN_USER_LED, LOW);
            // The STM32 started its internal IWDG here (watchdog_init()). Nothing
            // to start on this board: the TPL5010 is already running and is being
            // petted by the wdt task created before this one. See src/wdt.h.
            Serial.println("[app] init complete — insert coins, USER_LED lights when ready");
            vTaskDelay(pdMS_TO_TICKS(2000));  // hold the boot splash on screen before the idle screen takes over
            state = APP_STATE_IDLE;
            break;

        // ----------------------------------------------------------------
        // IDLE — Wait for Start Button (or Config Mode)
        //   PIN_USER_LED = ON when coins inserted >= coins_required (a peso
        //   price) ("ready")
        //   |- BTN1+BTN2 Long Press? -> Config Mode
        //   |- OP_AUTO_START && ready? -> Coin Validate automatically
        //   |- OP_PRESS_TO_START/OP_PAUSE_RESUME && ready too long? -> Coin Validate
        //   |   (shared inactivity_timeout_s safety net — see config.h)
        //   |- START tap + credits OK? -> Coin Validate
        // ----------------------------------------------------------------
        case APP_STATE_IDLE: {
            const uint32_t money_cents = coin_get_value_cents();
            const bool ready = (money_cents >= cfg.coins_required * 100UL);
            const bool auto_start = (cfg.operation_mode == OP_AUTO_START);
            const bool timeout_capable = (cfg.operation_mode == OP_PRESS_TO_START ||
                                          cfg.operation_mode == OP_PAUSE_RESUME);

            digitalWrite(PIN_USER_LED, ready ? HIGH : LOW);

            // Refresh display only when the ready/not-ready state changes
            if (money_cents != last_idle_money_cents) {
                last_idle_money_cents = money_cents;
                last_money_change_tick = xTaskGetTickCount();  // (re)start the Auto Start settle window
                if (auto_start) {
                    display_show_idle_auto(money_cents, cfg.coins_required * 100UL);
                } else if (ready) {
                    display_show_ready(money_cents);
                } else {
                    display_show_idle(money_cents);
                }
            }

            // ready_since_tick keeps resetting while not ready, so it only starts
            // measuring "how long have we been ready" from the moment ready begins —
            // extra coins inserted afterward do not restart the patience clock.
            if (!ready) {
                ready_since_tick = xTaskGetTickCount();
            }
            const bool inactivity_elapsed = timeout_capable && ready && cfg.inactivity_timeout_s > 0 &&
                (xTaskGetTickCount() - ready_since_tick) >= pdMS_TO_TICKS((uint32_t)cfg.inactivity_timeout_s * 1000);

            // Auto Start also waits out the settle window above — money_cents
            // must have stopped changing for AUTO_START_SETTLE_TICKS before
            // firing, so a whole deposit gets billed together (see comment above).
            const bool auto_start_settled = auto_start && ready &&
                (xTaskGetTickCount() - last_money_change_tick) >= AUTO_START_SETTLE_TICKS;

            if (btn_b1_b2_long_pressed()) {
                state = APP_STATE_CONFIG_MODE;
            } else if (start_button_pressed() || auto_start_settled || inactivity_elapsed) {
                DBG(inactivity_elapsed ? "[app] inactivity timeout — auto-start, coins_cents="
                    : auto_start        ? "[app] auto-start triggered — coins_cents="
                                        : "[app] button pressed — coins_cents=");
                DBGLN(money_cents);
                state = APP_STATE_COIN_VALIDATE;
            }
            watchdog_feed(WDT_SRC_APP);
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        // ----------------------------------------------------------------
        // Config Mode
        //   Blocking call to config_menu_run():
        //     BTN1/-  BTN2/+  START-tap=page  BTN1+BTN2-long=save  START-long=cancel
        //   If saved -> config_save() so it persists across reboot.
        //   Coin slot is force-disabled for the whole menu regardless of
        //   accumulation_enabled or operation_mode — no coins should be
        //   accepted while settings are being changed.
        // ----------------------------------------------------------------
        case APP_STATE_CONFIG_MODE:
            coin_slot_disable();
            buzzer_beep_enter_config();
            if (config_menu_run(&cfg)) {
                config_save(&cfg);
                coin_counter_set_price_per_credit(cfg.price_per_credit_cents);
                display_set_idle_pricing(cfg.coins_required, cfg.relay_on_ms,
                                          (cfg.operation_mode == OP_PRESS_TO_START ||
                                           cfg.operation_mode == OP_PAUSE_RESUME) && cfg.press_to_start_per_credit);
            }
            buzzer_beep_exit_config();
            coin_slot_enable();
            last_idle_money_cents = UINT32_MAX;  // force redraw on next IDLE tick
            state = APP_STATE_IDLE;
            break;

        // ----------------------------------------------------------------
        // Coin Validate — floor inserted money to whole credits, where
        // coins_required is the PESO PRICE of one credit
        // (coins_required * 100 centavos), not a raw pulse count.
        // relay_on_ms is the duration granted PER CREDIT —
        // e.g. coins_required=6 (P6.00/credit), relay_on_ms=1000: P38.00
        // inserted bills as 6 credits (P36.00) = 6 s; the leftover P2.00 is
        // carried over (not discarded) for the next credit, this session
        // (accumulation) or the next one — only the billed amount is
        // consumed, never a full reset.
        //   |- >=1 credit  -> snapshot billed credits, consume just that many, -> Session Active
        //   |- 0 credits   -> log + denied feedback + back to IDLE
        //
        // (OP_PRESS_TO_START or OP_PAUSE_RESUME) + press_to_start_per_credit: dispense
        // one credit per button press instead of all billed credits in one session —
        // session_credits is set to 1 for this round, remaining_credits tracks
        // how many more are still owed (drained in APP_STATE_SESSION_ACTIVE).
        // ----------------------------------------------------------------
        case APP_STATE_COIN_VALIDATE: {
            const uint32_t money_cents = coin_get_value_cents();
            const uint32_t price_cents = cfg.coins_required * 100UL;      // peso price of ONE credit, in centavos
            const uint32_t num_blocks  = (price_cents > 0) ? (money_cents / price_cents) : 0;  // integer division floors
            if (num_blocks >= 1) {
                const bool per_credit = ((cfg.operation_mode == OP_PRESS_TO_START ||
                                          cfg.operation_mode == OP_PAUSE_RESUME) &&
                                         cfg.press_to_start_per_credit);
                coin_consume_value_cents(num_blocks * price_cents);  // leftover cents are carried over, not reset
                digitalWrite(PIN_USER_LED, LOW);
                if (per_credit) {
                    remaining_credits = num_blocks;
                    session_credits   = 1;   // dispense just the first credit this round
                } else {
                    remaining_credits = 0;
                    session_credits   = num_blocks;  // dispense all billed credits in one session
                }
                DBG("[app] ");
                DBG(num_blocks);
                DBG(" credit(s) billed — relay total=");
                DBG(num_blocks * cfg.relay_on_ms);
                DBGLN(" ms");
                state = APP_STATE_SESSION_ACTIVE;
            } else {
                DBGLN("[app] insufficient credits — returning to idle");
                buzzer_beep_denied();
                display_show_denied();
                vTaskDelay(pdMS_TO_TICKS(600));  // hold the "INSUFFICIENT CREDIT" message so it is readable
                watchdog_feed(WDT_SRC_APP);
                session_credits        = 0;
                last_idle_money_cents  = UINT32_MAX;  // force idle redraw over the denied message
                state = APP_STATE_IDLE;
            }
            break;
        }

        // ----------------------------------------------------------------
        // Session Start -> Timer Mode (session.cpp)
        // session_run() dispatches internally on cfg.operation_mode — shared
        // setup/teardown (beeps, coin slot, LCD) stays common; only the
        // countdown loop button handling differs per mode.
        // If remaining_credits is still >0 after this dispense (per-credit
        // mode), go wait for the next button press instead of ending.
        // ----------------------------------------------------------------
        case APP_STATE_SESSION_ACTIVE: {
            uint32_t extra_credits = 0;
            session_run(&cfg, session_credits, &extra_credits);
            remaining_credits += extra_credits;  // accumulation queued during per-credit gating
            if (remaining_credits > 0) {
                --remaining_credits;
            }
            if (remaining_credits > 0) {
                digitalWrite(PIN_USER_LED, HIGH);  // more paid-for credit waiting — same "ready" signal as IDLE
                wait_next_credit_start  = xTaskGetTickCount();
                wait_next_credit_repost = wait_next_credit_start;
                display_show_next_credit(remaining_credits);
                state = APP_STATE_WAIT_NEXT_CREDIT;
            } else {
                state = APP_STATE_SESSION_END;
            }
            break;
        }

        // ----------------------------------------------------------------
        // (OP_PRESS_TO_START or OP_PAUSE_RESUME) + press_to_start_per_credit only —
        // one block just finished, more remain. Wait for another button press, or
        // the shared inactivity_timeout_s safety net ("Limit Between Credits"),
        // before dispensing the next one.
        // ----------------------------------------------------------------
        case APP_STATE_WAIT_NEXT_CREDIT: {
            const bool timeout_elapsed = cfg.inactivity_timeout_s > 0 &&
                (xTaskGetTickCount() - wait_next_credit_start) >= pdMS_TO_TICKS((uint32_t)cfg.inactivity_timeout_s * 1000);

            // Accumulation — same billing rule as session_run() mid-session
            // check: bill any newly-completed whole credit immediately instead
            // of leaving it banked until the next block starts, so the LCD
            // (and remaining_credits) reflect a coin dropped while waiting
            // right away rather than only after the customer presses/times
            // out into the next block.
            if (cfg.accumulation_enabled) {
                const uint32_t money_cents = coin_get_value_cents();
                const uint32_t price_cents = cfg.coins_required * 100UL;
                const uint32_t new_blocks  = (price_cents > 0) ? (money_cents / price_cents) : 0;
                if (new_blocks >= 1) {
                    coin_consume_value_cents(new_blocks * price_cents);
                    remaining_credits += new_blocks;
                    DBG("[app] credit added while waiting — +");
                    DBG(new_blocks);
                    DBG(" block(s), remaining=");
                    DBGLN(remaining_credits);
                    display_show_next_credit(remaining_credits);
                    wait_next_credit_repost = xTaskGetTickCount();  // avoid a redundant repost 1 s from now
                }
            }

            // Repost once per second so the display task 5 s no-message
            // heartbeat does not force-render the plain idle screen over this.
            if ((xTaskGetTickCount() - wait_next_credit_repost) >= pdMS_TO_TICKS(1000)) {
                wait_next_credit_repost = xTaskGetTickCount();
                display_show_next_credit(remaining_credits);
            }

            if (start_button_pressed() || timeout_elapsed) {
                DBG(timeout_elapsed ? "[app] next credit (timeout) — remaining="
                                    : "[app] next credit (button) — remaining=");
                DBGLN(remaining_credits);
                digitalWrite(PIN_USER_LED, LOW);  // dispensing again — same OFF signal as COIN_VALIDATE
                session_credits = 1;
                state = APP_STATE_SESSION_ACTIVE;
            }
            watchdog_feed(WDT_SRC_APP);
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        // ----------------------------------------------------------------
        // Session End
        //   -> Relay off (safety)
        //   -> Optional GSM report
        //   -> IDLE
        // ----------------------------------------------------------------
        case APP_STATE_SESSION_END:
            relay_off();
            if (cfg.gsm_reporting_enabled) {
                // TODO: gsm_report_send()  (stub on the STM32 too)
            }
            session_credits   = 0;
            remaining_credits = 0;   // defensive — should already be 0 by construction
            last_idle_money_cents = UINT32_MAX;  // force idle redraw
            DBGLN("[app] session end — insert coins for next vend");
            state = APP_STATE_IDLE;
            break;

        default:
            state = APP_STATE_INIT;
            break;
        }
    }
}
