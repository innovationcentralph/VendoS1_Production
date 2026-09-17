#include "diag.h"
#include "rtc.h"
#include "wdt.h"
#include "periph.h"
#include "config.h"
#include <string.h>

// One bit per code, indexed by code value. Five codes, so a u8 is plenty and
// raising is naturally idempotent — no list to scan, no duplicates possible.
static uint8_t  s_raised    = 0;
static bool     s_dirty     = false;
static uint32_t s_boot_ts   = 0;      // epoch at boot, 0 if the clock was unset
static bool     s_boot_unv  = true;

static inline uint8_t bit_for(uint8_t code) { return (uint8_t)(1u << (code - 1)); }

void diag_raise(uint8_t code) {
    if (code < 0x01 || code > DIAG_ERR_MAX) return;
    const uint8_t b = bit_for(code);
    if (s_raised & b) return;          // already raised — not news
    s_raised |= b;
    s_dirty = true;
}

void diag_clear_all() {
    if (s_raised == 0) return;
    s_raised = 0;
    s_dirty  = true;

    // Deliberately does NOT re-latch the boot conditions. `clear_errors` means
    // "I have recorded these server-side"; re-raising 0x05 immediately would
    // make it impossible to ever acknowledge a watchdog reset. 0x03 comes back
    // on its own via diag_service() if the clock is still unset, which is
    // correct — that one is a present condition, not a historical event.
}

uint8_t diag_sensors_bitmap() {
    uint8_t b = 0;

    AppConfig cfg;
    if (!config_load_quiet(&cfg)) config_defaults(&cfg);

    // The S1 has ONE generic sensor input (J6 AUX_1), not separate door and
    // vibration sensors — same situation as modules_bitmap in Device Info. It
    // reports as `door`, the app's generic slot; `vibration` is never set
    // rather than claiming an input that does not exist.
    if (sensor_read_active(cfg.sensor_active_high)) b |= (uint8_t)(1u << DIAG_SENSOR_BIT_DOOR);

    // Live coin line, interpreted through the configured polarity — so this bit
    // means "a coin pulse is being asserted right now", not "IO17 is high".
    // This is what lets a technician watch the bit toggle as a coin drops.
    if (coin_raw_level() == coin_counter_polarity()) b |= (uint8_t)(1u << DIAG_SENSOR_BIT_COIN);

    // Not a sensor in the physical sense: "the clock is set and trustworthy".
    if (rtc_valid()) b |= (uint8_t)(1u << DIAG_SENSOR_BIT_RTC);

#ifdef ENABLE_DIAG_BUTTON_BIT
    // Live user-button level, so the app can WAIT on a press rather than poll
    // for one. Paired with the change detection in diag_service(), which is
    // what actually pushes the notification. See diag.h on why this is gated.
    if (user_btn_raw_pressed()) b |= (uint8_t)(1u << DIAG_SENSOR_BIT_BUTTON);
#endif

    return b;
}

static void put_u32(uint8_t* b, size_t off, uint32_t v) {
    b[off]     = (uint8_t)(v & 0xFF);
    b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

// Best-effort boot timestamp.
//
// If the clock was valid at boot we stamped it then, and it is exact. If it was
// not, but has since been set, we back-calculate boot_ts = now - uptime, which
// is far more useful to the backend than 0 — and we keep reporting
// boot_ts_unverified = 1, because the boot instant itself was never observed
// against a real clock. The flag is about provenance, not arithmetic.
static uint32_t boot_ts_now(uint32_t uptime_s, bool* unverified) {
    if (s_boot_ts != 0) { *unverified = s_boot_unv; return s_boot_ts; }

    const uint32_t now = rtc_now();
    if (now != 0 && now > uptime_s) { *unverified = true; return now - uptime_s; }

    *unverified = true;
    return 0;
}

size_t diag_serialize(uint8_t* out, size_t cap) {
    if (cap < DIAG_WIRE_HEADER_LEN) return 0;

    const uint32_t uptime_s = millis() / 1000UL;
    bool unverified = true;
    const uint32_t boot_ts = boot_ts_now(uptime_s, &unverified);

    put_u32(out, 0, uptime_s);
    put_u32(out, 4, boot_ts);
    out[8] = unverified ? 1 : 0;
    out[9] = diag_sensors_bitmap();

    size_t n = DIAG_WIRE_HEADER_LEN;
    uint8_t count = 0;
    for (uint8_t code = 0x01; code <= DIAG_ERR_MAX; ++code) {
        if (!(s_raised & bit_for(code))) continue;
        if (n >= cap) break;
        out[n++] = code;
        ++count;
    }
    out[10] = count;
    return n;
}

bool diag_take_dirty() {
    const bool d = s_dirty;
    s_dirty = false;
    return d;
}

void diag_service() {
    // 0x03 is a *present condition*, not an event: if the clock is still unset
    // it belongs in the list even after a clear_errors. Re-raising is free —
    // diag_raise() is idempotent and only marks dirty on a real transition.
    if (!rtc_valid()) diag_raise(DIAG_ERR_RTC_UNSET);

#ifdef ENABLE_DIAG_BUTTON_BIT
    // Mark the frame dirty on a button EDGE so ble_diagnostics_service() pushes
    // a notification and the app can wait on a press instead of polling for it.
    //
    // Reads the pin directly rather than calling diag_sensors_bitmap(): that
    // function does a config_load_quiet() for the sensor polarity, and an NVS
    // read on this 10 ms path would be absurd. Edge-triggered, so an idle board
    // still sends nothing; a press/release pair is two frames per session,
    // which is nothing next to the per-coin frames f003 already sends.
    static bool s_btn_last = false;
    const bool  now = user_btn_raw_pressed();
    if (now != s_btn_last) {
        s_btn_last = now;
        s_dirty    = true;
    }
#endif
}

void diag_init() {
    // Watchdog reset. See wdt.h: ESP_RST_EXT is ambiguous on this board (the
    // TPL5010 and SW1 share ESP_EN) and is counted as a watchdog reset anyway,
    // because in the field SW1 is behind a locked door.
    if (wdt_boot_was_watchdog()) diag_raise(DIAG_ERR_WDT_RESET);

    if (rtc_valid()) {
        s_boot_ts  = rtc_now();
        s_boot_unv = false;
    } else {
        s_boot_ts  = 0;
        s_boot_unv = true;
        diag_raise(DIAG_ERR_RTC_UNSET);
    }
}

void diag_print(Stream& out) {
    const uint32_t uptime_s = millis() / 1000UL;
    bool unverified = true;
    const uint32_t boot_ts = boot_ts_now(uptime_s, &unverified);

    out.print("uptime   = "); out.print(uptime_s); out.println(" s");
    out.print("boot_ts  = "); out.print(boot_ts);
    out.println(unverified ? "  (UNVERIFIED — clock was not set at boot)" : "  (verified)");

    const uint8_t sb = diag_sensors_bitmap();
    out.print("sensors  = 0x"); out.println(sb, HEX);
    out.print("  door(generic sensor) = "); out.println((sb >> DIAG_SENSOR_BIT_DOOR) & 1);
    out.print("  vibration            = 0  (no distinct input on the S1)");
    out.println();
    out.print("  coin line active     = "); out.println((sb >> DIAG_SENSOR_BIT_COIN) & 1);
    out.print("  rtc valid            = "); out.println((sb >> DIAG_SENSOR_BIT_RTC) & 1);

    out.println("errors:");
    if (s_raised == 0) {
        out.println("  (none)");
    } else {
        static const char* kNames[DIAG_ERR_MAX + 1] = {
            "", "coin pulse line fault", "relay drive fault",
            "RTC unset / battery-backup failure", "NVS write failure",
            "watchdog reset since last sync",
        };
        for (uint8_t code = 0x01; code <= DIAG_ERR_MAX; ++code) {
            if (!(s_raised & bit_for(code))) continue;
            out.print("  0x0"); out.print(code); out.print("  "); out.println(kNames[code]);
        }
        out.println("Cleared by the app's clear_errors op after it uploads them.");
        out.println("Until f007 Command exists, AT+DIAG_CLEAR is the only way to empty this.");
    }

    if (wdt_boot_was_watchdog() && wdt_boot_reason() == ESP_RST_EXT) {
        out.println("NOTE: 0x05 came from an EN-pin reset, which is the TPL5010 OR SW1 —");
        out.println("      indistinguishable. If you pressed RESET, this is expected.");
    }
}
