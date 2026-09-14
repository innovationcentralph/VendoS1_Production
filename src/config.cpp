#include "config.h"
#include <Preferences.h>
#include "diag.h"
#include <Arduino.h>

// =============================================================================
// Implemented operation modes — see the extern declaration in config.h for why
// this must not be assumed contiguous with [0, NUM_OPERATION_MODES_IMPLEMENTED).
// =============================================================================
const uint8_t kImplementedOpModes[NUM_OPERATION_MODES_IMPLEMENTED] = {
    OP_PRESS_TO_START,
    OP_PAUSE_RESUME,
    OP_AUTO_START,
};

// =============================================================================
// Factory defaults — value-for-value identical to the STM32 firmware, with the
// one appended ESP32-only field at the end.
// =============================================================================
static const AppConfig kDefaults = {
    APP_CONFIG_MAGIC,
    5000,                    // relay_on_ms
    1,                       // coins_required
    OP_PRESS_TO_START,       // operation_mode
    false,                   // press_to_start_per_credit
    0,                       // inactivity_timeout_s
    false,                   // accumulation_enabled
    false,                   // sensor_stop_enabled
    false,                   // sensor_active_high
    50,                      // sensor_wait_ms
    false,              // gsm_reporting_enabled
    DISPLAY_LCD_16X2,   // display_type
    1000,               // beep_start_freq_hz
    2,                  // beep_start_count
    150,                // beep_start_on_ms
    2500,               // beep_end_freq_hz
    3,                  // beep_end_count
    70,                 // beep_end_on_ms
    80,                 // beep_off_ms
    1000,               // price_per_credit_cents (P10.00)
    true,               // coin_active_high — idle LOW, pulse HIGH (see config.h)
};

void config_defaults(AppConfig* cfg) {
    *cfg = kDefaults;
}

// =============================================================================
// NVS load / save
//
// One Preferences handle opened and closed per call rather than held open. The
// config is touched at boot and on a menu save — a handful of times in a board
// lifetime — so there is nothing to gain from keeping it open, and a handle
// left open is a handle that can be left open across a reset.
// =============================================================================
bool config_load_quiet(AppConfig* cfg) {
    Preferences prefs;
    AppConfig   stored;

    if (!prefs.begin(APP_CONFIG_NS, /*readOnly=*/true)) return false;

    // getBytes returns the number of bytes actually copied. A short read means a
    // layout change or a truncated write — treat it exactly like a bad magic.
    const size_t n = prefs.getBytes(APP_CONFIG_KEY, &stored, sizeof(stored));
    prefs.end();

    if (n != sizeof(stored) || stored.magic != APP_CONFIG_MAGIC) return false;

    *cfg = stored;
    return true;
}

bool config_load(AppConfig* cfg) {
    AppConfig stored;
    const bool valid = config_load_quiet(&stored);

    if (!valid) {
        Serial.println("[cfg] NVS blank or stale — writing defaults");
        config_defaults(cfg);
        config_save(cfg);
        config_print(cfg, Serial);
        return false;
    }

    *cfg = stored;
    Serial.println("[cfg] config loaded from NVS");
    config_print(cfg, Serial);
    return true;
}

void config_save(const AppConfig* cfg) {
    AppConfig to_save = *cfg;
    to_save.magic = APP_CONFIG_MAGIC;

    Preferences prefs;
    if (!prefs.begin(APP_CONFIG_NS, /*readOnly=*/false)) {
        Serial.println("[cfg] ERROR: NVS open failed — config NOT saved");
        // ESP32-only: surface it to the app. Serial goes nowhere in a deployed
        // machine, and the operator's symptom is "the price I set reverted".
        diag_raise(DIAG_ERR_NVS_WRITE);
        return;
    }
    const size_t n = prefs.putBytes(APP_CONFIG_KEY, &to_save, sizeof(to_save));
    prefs.end();

    if (n == sizeof(to_save)) {
        Serial.println("[cfg] config saved to NVS");
    } else {
        // Worth shouting about rather than swallowing: a silent failure here
        // means the operator thinks they changed a price and did not.
        Serial.print("[cfg] ERROR: short NVS write (");
        Serial.print((unsigned)n); Serial.print('/');
        Serial.print((unsigned)sizeof(to_save));
        Serial.println(" bytes) — config NOT saved");
        diag_raise(DIAG_ERR_NVS_WRITE);   // ESP32-only, as above
    }
}

// =============================================================================
// Debug helpers
// =============================================================================
void config_print(const AppConfig* cfg, Stream& out) {
    out.println("--- AppConfig (parsed) ---");
    out.print("  magic              = 0x"); out.print(cfg->magic, HEX);
    out.println(cfg->magic == APP_CONFIG_MAGIC ? "  [VALID]" : "  [*** INVALID — defaults in use ***]");
    out.print("  relay_on_ms        = "); out.print(cfg->relay_on_ms);        out.println(" ms");
    out.print("  coins_required     = "); out.println(cfg->coins_required);
    out.print("  operation_mode     = ");
    switch (cfg->operation_mode) {
        case OP_PRESS_TO_START: out.println("0 (Press to Start)");   break;
        case OP_PAUSE_RESUME:   out.println("1 (Pause/Resume)");     break;
        case OP_ON_OFF_TOGGLE:  out.println("2 (ON/OFF Toggle)");    break;
        case OP_AUTO_START:     out.println("3 (Auto Start)");       break;
        case OP_SENSOR_STOP:    out.println("4 (reserved, unused)"); break;
        default:                out.println("? (unknown)");          break;
    }
    if (cfg->operation_mode == OP_PRESS_TO_START || cfg->operation_mode == OP_PAUSE_RESUME) {
        out.print("  per_credit_mode    = "); out.println(cfg->press_to_start_per_credit ? "ON" : "OFF");
    }
    out.print("  inactivity_timeout = "); out.print(cfg->inactivity_timeout_s);
    out.println(cfg->inactivity_timeout_s == 0 ? " (disabled)" : " s");
    out.print("  accumulation       = "); out.println(cfg->accumulation_enabled ? "ON" : "OFF");
    out.print("  sensor_stop        = "); out.println(cfg->sensor_stop_enabled ? "ON" : "OFF");
    if (cfg->sensor_stop_enabled) {
        out.print("  sensor_active_high = "); out.println(cfg->sensor_active_high ? "HIGH" : "LOW");
        out.print("  sensor_wait_ms     = "); out.print(cfg->sensor_wait_ms);
        out.println(cfg->sensor_wait_ms == 0 ? " (immediate)" : " ms");
    }
    out.print("  gsm_reporting      = "); out.println(cfg->gsm_reporting_enabled   ? "ON" : "OFF");
    out.print("  display_type       = ");
    switch (cfg->display_type) {
        case DISPLAY_LCD_16X2:  out.println("1 (LCD 16x2 I2C)");         break;
        case DISPLAY_7SEG_4DIG: out.println("2 (4-digit 7-segment)");    break;
        default:                out.println("0 (none)");                  break;
    }
    out.print("  beep_start         = "); out.print(cfg->beep_start_freq_hz); out.print(" Hz x");
    out.print(cfg->beep_start_count); out.print(", on="); out.print(cfg->beep_start_on_ms); out.println(" ms");
    out.print("  beep_end           = "); out.print(cfg->beep_end_freq_hz);   out.print(" Hz x");
    out.print(cfg->beep_end_count); out.print(", on="); out.print(cfg->beep_end_on_ms); out.println(" ms");
    out.print("  beep_off_ms        = "); out.print(cfg->beep_off_ms); out.println(" ms");
    out.print("  price_per_credit   = ");
    out.print(cfg->price_per_credit_cents / 100); out.print('.');
    const uint32_t price_frac = cfg->price_per_credit_cents % 100;
    if (price_frac < 10) out.print('0');
    out.println(price_frac);
    // ESP32-only — see config.h.
    out.print("  coin_active_high   = ");
    out.println(cfg->coin_active_high ? "HIGH (idle LOW, pulse HIGH)"
                                      : "LOW (idle HIGH, pulse LOW)");
}

void config_dump_eeprom(Stream& out) {
    const size_t n = sizeof(AppConfig);

    out.println("=== stored config dump (NVS) ===");
    out.print("Namespace/key: "); out.print(APP_CONFIG_NS); out.print('/'); out.print(APP_CONFIG_KEY);
    out.print("  ("); out.print(n); out.println(" bytes)");

    AppConfig stored;
    size_t    got = 0;
    Preferences prefs;
    if (prefs.begin(APP_CONFIG_NS, /*readOnly=*/true)) {
        got = prefs.getBytes(APP_CONFIG_KEY, &stored, sizeof(stored));
        prefs.end();
    }

    if (got != n) {
        out.print("No valid blob stored (read ");
        out.print((unsigned)got); out.print('/'); out.print((unsigned)n);
        out.println(" bytes) — defaults will be used at boot");
        out.println("================================");
        return;
    }

    // --- Raw hex bytes ---
    out.println("Raw bytes (hex):");
    out.print("  ");
    const uint8_t* raw = (const uint8_t*)&stored;
    for (size_t i = 0; i < n; ++i) {
        if (raw[i] < 0x10) out.print('0');
        out.print(raw[i], HEX);
        out.print(' ');
        if (((i + 1) % 16) == 0) {
            out.println();
            out.print("  ");
        }
    }
    out.println();

    config_print(&stored, out);
    out.println("================================");
}
