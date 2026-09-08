#include "session.h"
#include "periph.h"
#include "display.h"
#include "debug_log.h"
#include "wdt.h"
#include <Arduino.h>

// Poll period for the countdown loop — matches the IDLE loop 20 ms cadence
// so OP_PAUSE_RESUME button-toggle debounce (3 samples) confirms in ~60 ms.
static const uint32_t SESSION_TICK_MS = 20;

// The STM32 relied on configTICK_RATE_HZ being 1000, so that 1 tick == 1 ms and
// pdMS_TO_TICKS was the identity. Arduino-ESP32 also configures 1000 Hz, and the
// tick-arithmetic below (especially SESSION_TICKS_CAP) is only correct under that
// assumption — so assert it rather than inherit it silently. If a future core
// changes the default, this breaks the build instead of quietly mis-timing a vend.
static_assert(configTICK_RATE_HZ == 1000,
              "session timing assumes 1 tick == 1 ms; revisit SESSION_TICKS_CAP");

// Defensive ceiling on how far Accumulation can extend session_ticks within
// one continuous session. TickType_t is a uint32_t counting ms (1 tick = 1 ms
// at the configured tick rate), which wraps at ~49.7 days — not reachable
// under any realistic coin-drop volume, but with no cap enough back-to-back
// accumulated credits (relay_on_ms up to 3 600 000 ms per AT+RELAY_MS) could
// theoretically get there over a long-running session that keeps extending
// itself before it can naturally expire. Purely a wraparound backstop, not a
// business-facing limit.
// Written directly in ticks (NOT via pdMS_TO_TICKS) — that macro computes
// ((ms * configTICK_RATE_HZ) / 1000) in 32-bit TickType_t, and a 7-day ms
// value overflows during the multiply before the divide ever runs. Since
// configTICK_RATE_HZ is 1000 (1 tick = 1 ms), the tick count IS the ms count,
// so this is written as the equivalent direct multiplication (which stays
// well under 2^32) instead.
static const TickType_t SESSION_TICKS_CAP = 7UL * 24 * 3600 * 1000; // 7 days

// Bills any newly-banked whole credits (AppConfig::accumulation_enabled)
// against the running session — queuing them as extra gated credits under
// per-credit gating, or extending session_ticks live otherwise (clamped to
// SESSION_TICKS_CAP above). session_run() calls this at two points per loop
// iteration — before AND after vTaskDelay — so a coin landing during that
// sleep, in what would otherwise be the final iteration before natural
// expiry, still gets applied before the loop exit condition is re-checked,
// instead of silently missing this session and only carrying over to the
// next one.
static void session_apply_accumulation(const AppConfig* cfg, bool per_credit_gated,
                                        TickType_t session_start, TickType_t* session_ticks,
                                        TickType_t* last_status_tick,
                                        uint32_t* extra_credits_out) {
    const uint32_t money_cents = coin_get_value_cents();
    const uint32_t price_cents = cfg->coins_required * 100UL;
    const uint32_t new_blocks  = (price_cents > 0) ? (money_cents / price_cents) : 0;
    if (new_blocks < 1) return;

    coin_consume_value_cents(new_blocks * price_cents);

    if (per_credit_gated) {
        if (extra_credits_out) *extra_credits_out += new_blocks;
        DBG("[session] credit added — +");
        DBG(new_blocks);
        DBGLN(" block(s) queued (per-credit gating — requires button press)");
        return;
    }

    const uint32_t   extend_ms = new_blocks * cfg->relay_on_ms;
    const TickType_t new_total = *session_ticks + pdMS_TO_TICKS(extend_ms);
    *session_ticks = (new_total > SESSION_TICKS_CAP) ? SESSION_TICKS_CAP : new_total;

    const TickType_t elapsed_now   = xTaskGetTickCount() - session_start;
    const uint32_t   remaining_now = (elapsed_now < *session_ticks)
        ? (uint32_t)((*session_ticks - elapsed_now) * portTICK_PERIOD_MS)
        : 0;
    DBG("[session] credit added — +");
    DBG(new_blocks);
    DBG(" block(s), +");
    DBG(extend_ms);
    DBG(" ms, remaining=");
    DBG(remaining_now);
    DBGLN(" ms");
    display_show_session(remaining_now);
    *last_status_tick = xTaskGetTickCount();
}

// =============================================================================
// session_run — Timer Mode
//
// Caller has already satisfied one or more credit blocks. This function:
//   1. Beeps START tone
//   2. Turns relay + LEDs ON
//   3. Counts down blocks * cfg->relay_on_ms (yielding every SESSION_TICK_MS)
//   4. Turns relay + LEDs OFF
//   5. Beeps END tone
//   6. Returns — control goes back to APP_STATE_SESSION_END
// =============================================================================
void session_run(const AppConfig* cfg, uint32_t blocks, uint32_t* extra_credits_out) {

    // Per-credit gating: (OP_PRESS_TO_START or OP_PAUSE_RESUME) + press_to_start_per_credit
    // dispenses one block per button press. Accumulation must not stretch the block
    // that is actively running in that case — new coins are queued as extra gated
    // credits (via extra_credits_out) instead of extending session_ticks below.
    const bool per_credit_gated = ((cfg->operation_mode == OP_PRESS_TO_START ||
                                     cfg->operation_mode == OP_PAUSE_RESUME) &&
                                    cfg->press_to_start_per_credit);

    // -------------------------------------------------------------------------
    // Session Start — Beep + Relay ON
    // Total time = blocks x relay_on_ms (relay_on_ms is per BLOCK, not per raw credit)
    // -------------------------------------------------------------------------
    const uint32_t total_ms = blocks * cfg->relay_on_ms;

    DBG("[session] START — ");
    DBG(blocks);
    DBG(" block(s) x ");
    DBG(cfg->relay_on_ms);
    DBG(" ms = ");
    DBG(total_ms);
    DBGLN(" ms total");

    // Post the display update BEFORE the relay fires so the first I2C write
    // completes while the bus is still clean (relay coil EMI can corrupt I2C).
    display_show_session(total_ms);
    vTaskDelay(pdMS_TO_TICKS(20));    // give the display task time to execute the write

    buzzer_beep_session_start(cfg);
    relay_on();
    leds_set(true);
    bool relay_engaged = true;   // OP_PAUSE_RESUME toggle state — starts ON
    if (!cfg->accumulation_enabled) {
        coin_slot_disable();   // inhibit coin slot for the whole session, pause or not
        DBGLN("[session] coin slot disabled (accumulation OFF)");
    } else {
        // Accumulation needs the coin slot to stay live for the "add time while
        // running" feature below.
        DBGLN("[session] coin slot stays enabled (accumulation ON)");
    }

    // -------------------------------------------------------------------------
    // Timer Count Down
    // OP_PRESS_TO_START : button ignored, no pause (default)
    // OP_PAUSE_RESUME   : button toggles relay ON/OFF; timer is NEVER paused —
    //                     the customer keeps consuming purchased time even
    //                     while the relay is OFF (per spec).
    // Accumulation (accumulation_enabled) and Sensor Stop (sensor_stop_enabled)
    // are both composable with ANY operation_mode above — Accumulation lets
    // mid-session coins extend the deadline (see extend block below); Sensor
    // Stop is an additional early-stop condition checked alongside the timer
    // deadline. Neither is a mutually-exclusive mode of its own.
    // -------------------------------------------------------------------------
    // Use an absolute tick deadline so drift from vTaskDelay overshoot does not
    // accumulate. TickType_t subtraction is unsigned and wraps correctly.
    // session_ticks is mutable — accumulation_enabled extends it as credit is added.
    const TickType_t session_start    = xTaskGetTickCount();
    TickType_t       session_ticks    = pdMS_TO_TICKS(total_ms);
    TickType_t       last_status_tick = session_start;

    // Sensor Stop trigger-timing state (see AppConfig::sensor_wait_ms).
    // sensor_active_ms accumulates SESSION_TICK_MS per consecutive tick the
    // sensor reads active, and resets to 0 the moment it reads inactive — this
    // is what makes sensor_wait_ms require *continuous* detection, not just
    // cumulative flickers. sensor_wait_ms == 0 fires on the first active tick.
    bool     sensor_stop_fired = false;
    uint32_t sensor_active_ms  = 0;

    while (!sensor_stop_fired && (xTaskGetTickCount() - session_start) < session_ticks) {
        // Liveness check-in. On the STM32 this call kicked the IWDG directly;
        // here the wdt task owns petting and this only records progress. Kept at
        // the same place in the loop so the two codebases stay diffable, and so
        // the pending supervision work (docs/PENDING.md item 1) has the session
        // countdown already instrumented. See src/wdt.h.
        watchdog_feed(WDT_SRC_SESSION);

        if (cfg->operation_mode == OP_PAUSE_RESUME && start_button_pressed()) {
            relay_engaged = !relay_engaged;
            relay_engaged ? relay_on() : relay_off();
            leds_set(relay_engaged);
            DBGLN(relay_engaged ? "[session] button — relay ON (resumed)"
                                 : "[session] button — relay OFF (paused, timer still running)");
        }

        // Accumulation — floor newly-inserted money to whole credits (same
        // money-based quantization rule as COIN_VALIDATE: coins_required is
        // the peso price of one credit). Leftover cents are left alone so they
        // can complete a credit later in this session or the next one.
        // Composable with any operation_mode (see accumulation_enabled).
        //
        // Under per-credit gating (see per_credit_gated above), the newly
        // billed blocks are queued as extra gated credits instead of
        // extending the block currently dispensing — otherwise a coin dropped
        // mid-block would silently stretch it, defeating the one-press-per-
        // credit gate. See session_apply_accumulation() doc comment above
        // for why this is also called again below, after vTaskDelay.
        if (cfg->accumulation_enabled) {
            session_apply_accumulation(cfg, per_credit_gated, session_start, &session_ticks,
                                        &last_status_tick, extra_credits_out);
        }

        // Sensor Stop — composable with any operation_mode. Ends the session
        // early (relay OFF happens in the shared teardown below, same as a
        // normal timer expiry) once the sensor has read continuously active
        // for at least sensor_wait_ms (0 = immediate, fires on the first tick).
        if (cfg->sensor_stop_enabled) {
            if (sensor_read_active(cfg->sensor_active_high)) {
                sensor_active_ms += SESSION_TICK_MS;
                if (sensor_active_ms >= cfg->sensor_wait_ms) {
                    sensor_stop_fired = true;
                    DBG("[session] sensor triggered — ending early after ");
                    DBG(sensor_active_ms);
                    DBGLN(" ms active");
                }
            } else {
                sensor_active_ms = 0;   // needs continuous detection — any gap resets it
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SESSION_TICK_MS));

        // Re-check for coins that landed during the delay above — otherwise
        // one landing in what turns out to be the final iteration before
        // natural expiry would be banked but never evaluated against the
        // deadline before the loop exits on the next `while` check.
        if (cfg->accumulation_enabled) {
            session_apply_accumulation(cfg, per_credit_gated, session_start, &session_ticks,
                                        &last_status_tick, extra_credits_out);
        }

        const TickType_t elapsed           = xTaskGetTickCount() - session_start;
        const uint32_t   time_remaining_ms = (elapsed < session_ticks)
            ? (uint32_t)((session_ticks - elapsed) * portTICK_PERIOD_MS)
            : 0;

        // Update serial log + display every 1 s
        if ((xTaskGetTickCount() - last_status_tick) >= pdMS_TO_TICKS(1000)) {
            last_status_tick = xTaskGetTickCount();
            DBG("[session] remaining=");
            DBG(time_remaining_ms);
            DBGLN(" ms");
            display_show_session(time_remaining_ms);
        }
    }

    // -------------------------------------------------------------------------
    // Session End — Relay OFF + End Beep
    // Unconditional relay_off() here also covers OP_PAUSE_RESUME: timer expiry
    // always ends the transaction regardless of whatever toggle state the
    // relay was left in.
    // -------------------------------------------------------------------------
    relay_off();
    leds_set(false);
    coin_slot_enable();   // always re-enable — safe regardless of the accumulation setting
    DBGLN("[session] relay OFF, coin slot enabled");
    display_show_done();

    buzzer_beep_session_end(cfg);

    vTaskDelay(pdMS_TO_TICKS(3000));  // hold "COMPLETE!" so it is clearly readable before IDLE takes over
    watchdog_feed(WDT_SRC_SESSION);

    DBGLN("[session] complete");
}
