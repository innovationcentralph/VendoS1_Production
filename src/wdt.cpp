#include "wdt.h"
#include "pins.h"
#include <esp_system.h>

// =============================================================================
// State
// =============================================================================
static uint32_t s_petCount = 0;

// Liveness check-ins. 0 means "this source has never checked in", which is not
// the same as "stalled" — see watchdog_liveness_stale_ms(). volatile because
// they are written from the app/session/menu tasks and read from the wdt task.
static volatile uint32_t s_lastFeedMs[WDT_SRC_COUNT] = { 0 };

static esp_reset_reason_t s_resetReason = ESP_RST_UNKNOWN;

// =============================================================================
// DONE pulse
// =============================================================================
static inline void pulse_done() {
    // Edge-triggered pet. Returns to LOW immediately and stays there: IO2 is a
    // strapping pin, so it must never be sitting high if a reset lands.
    digitalWrite(PIN_WDT_DONE, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_WDT_DONE, LOW);
    ++s_petCount;
}

void wdt_begin() {
    // Latch the reset reason before anything else can obscure it.
    s_resetReason = esp_reset_reason();

    digitalWrite(PIN_WDT_DONE, LOW);        // before pinMode, so the pad never
    pinMode(PIN_WDT_DONE, OUTPUT);          // glitches high on the way to being
    digitalWrite(PIN_WDT_DONE, LOW);        // an output
    pulse_done();
}

void wdt_pet() {
    pulse_done();
}

// =============================================================================
// Petting task
//
// vTaskDelayUntil, not vTaskDelay: the pet cadence must not drift with however
// long the pulse and the loop body took. At a 100 ms period against a possible
// ~100 ms hardware window there is no margin to give away.
// =============================================================================
void wdt_task_run(void* arg) {
    (void)arg;

    Serial.print("[wdt] TPL5010 petting task running — DONE on IO");
    Serial.print(PIN_WDT_DONE);
    Serial.print(" every ");
    Serial.print(WDT_PET_MS);
    Serial.println(" ms");

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        pulse_done();

        // PENDING (docs/PENDING.md item 1): this is where application-level
        // supervision goes, restoring what the STM32 IWDG gave us for free:
        //
        //     if (watchdog_liveness_stale_ms() > WDT_LIVENESS_DEADLINE_MS) {
        //         // stop petting — the TPL5010 resets the board, GPIOs revert
        //         // to inputs, and R19 10K pulls the relay gate low
        //         for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        //     }
        //
        // Held back until the real TPL5010 timeout is measured (PENDING item 2)
        // and a deadline can be chosen from bench numbers rather than guessed.
        // A deadline set too tight reboots the board mid-vend, which is worse
        // than the supervision gap it would close.

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WDT_PET_MS));
    }
}

// =============================================================================
// Liveness
// =============================================================================
void watchdog_feed(WdtSource src) {
    if (src >= WDT_SRC_COUNT) return;
    const uint32_t now = millis();
    // millis() returns 0 only in the first millisecond after boot, and 0 is the
    // "never checked in" sentinel — clamp so a check-in that early is not read
    // as an absent one.
    s_lastFeedMs[src] = (now == 0) ? 1 : now;
}

uint32_t watchdog_liveness_stale_ms() {
    const uint32_t now = millis();
    uint32_t worst = 0;
    for (uint8_t i = 0; i < WDT_SRC_COUNT; ++i) {
        const uint32_t last = s_lastFeedMs[i];
        if (last == 0) continue;            // never ran; not the same as stalled
        const uint32_t age = now - last;    // unsigned, wraps correctly
        if (age > worst) worst = age;
    }
    return worst;
}

// =============================================================================
// Reporting
// =============================================================================
static const char* reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON (cold start / 12V applied)";
        // The TPL5010 nRST and the SW1 reset button both land here — they share
        // the ESP_EN net, so the ESP32 cannot tell them apart.
        case ESP_RST_EXT:       return "EXT (EN pin: watchdog nRST or SW1)";
        case ESP_RST_SW:        return "SW (esp_restart — e.g. AT+RESET)";
        case ESP_RST_PANIC:     return "PANIC (exception / crash)";
        case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
        case ESP_RST_WDT:       return "WDT (other internal watchdog)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (3.3V rail sagged)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wake";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

bool wdt_boot_was_watchdog() {
    switch (s_resetReason) {
        case ESP_RST_EXT:        // TPL5010 *or* SW1 — indistinguishable, see wdt.h
        case ESP_RST_WDT:        // unambiguous: an on-chip watchdog fired
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
            return true;
        default:
            return false;
    }
}

esp_reset_reason_t wdt_boot_reason() { return s_resetReason; }

void wdt_print_boot_report(Stream& out) {
    out.print("reset reason: ");
    out.println(reset_reason_name(s_resetReason));

    if (s_resetReason == ESP_RST_EXT) {
        out.println("  (EN-pin reset: either the TPL5010 fired or SW1 was pressed —");
        out.println("   the ESP32 cannot distinguish them. If nobody touched SW1,");
        out.println("   treat this as a watchdog reset and check what blocked for");
        out.println("   longer than the TPL5010 window.)");
    }
}

void wdt_print_status(Stream& out) {
    out.print("pet interval = "); out.print(WDT_PET_MS);   out.println(" ms (unconditional)");
    out.print("pets         = "); out.println(s_petCount);
    out.print("uptime       = "); out.print(millis() / 1000); out.println(" s");
    out.print("liveness age = "); out.print(watchdog_liveness_stale_ms());
    out.println(" ms (worst check-in; NOT enforced yet — see docs/PENDING.md)");
    for (uint8_t i = 0; i < WDT_SRC_COUNT; ++i) {
        static const char* kNames[WDT_SRC_COUNT] = { "app", "session", "menu" };
        out.print("  "); out.print(kNames[i]); out.print(" = ");
        if (s_lastFeedMs[i] == 0) out.println("never checked in");
        else { out.print(millis() - s_lastFeedMs[i]); out.println(" ms ago"); }
    }
    out.print("boot         = ");
    out.println(reset_reason_name(s_resetReason));
}

uint32_t wdt_pet_count() { return s_petCount; }
