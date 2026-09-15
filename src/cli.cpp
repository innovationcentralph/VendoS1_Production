#include "cli.h"
#include "config.h"
#include "identity.h"
#include "rtc.h"
#include "counters.h"
#include "eventlog.h"
#include "diag.h"
#include "periph.h"
#include "display.h"
#include "wdt.h"
#include "version.h"
#include <esp_system.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// ============================================================================
// Helpers
// ============================================================================

// Read the current stored config into *cfg.
// Works even before the app task has run — reads NVS directly.
//
// Note the same caveat the STM32 had: every setter below edits the STORED copy
// and tells you to AT+RESET. It does NOT reach into the app task live AppConfig,
// so a setting changed here takes effect on the next boot. The LCD menu is the
// path that applies immediately.
static void stored_read_cfg(AppConfig* cfg) {
    if (!config_load_quiet(cfg)) {
        config_defaults(cfg);
    }
}

// ============================================================================
// Command handlers
// ============================================================================

static void cmd_help(const char* args, Stream& out) {
    (void)args;
    size_t n = 0;
    const CliCommand* cmds = cli_get_commands(n);
    out.println("--- Supported AT commands ---");
    for (size_t i = 0; i < n; ++i) {
        out.print("  ");
        out.print(cmds[i].name);
        out.print("  -  ");
        out.println(cmds[i].help);
    }
    out.println("OK");
}

static void cmd_ping(const char* args, Stream& out) {
    (void)args;
    out.println("PONG");
    out.println("OK");
}

// AT+VERSION?  — report firmware version
static void cmd_version(const char* args, Stream& out) {
    (void)args;
    out.print("firmware_version = "); out.println(FW_VERSION_STRING);
    out.println("OK");
}

// AT+EEPROM?  — raw hex dump + re-parsed fields
// (Named for the STM32 original. The storage is NVS here, not emulated EEPROM.)
static void cmd_eeprom_dump(const char* args, Stream& out) {
    (void)args;
    config_dump_eeprom(out);
    out.println("OK");
}

// AT+CFG?  — parsed fields only (re-read from storage)
static void cmd_cfg_show(const char* args, Stream& out) {
    (void)args;
    AppConfig cfg;
    stored_read_cfg(&cfg);
    config_print(&cfg, out);
    out.println("OK");
}

// AT+CREDITS?  — current in-RAM credit count and money total
static void cmd_credits(const char* args, Stream& out) {
    (void)args;
    out.print("CREDITS: ");
    out.println(coin_get_count());
    out.print("VALUE:   ");
    out.print(coin_get_value_cents() / 100); out.print('.');
    const uint32_t frac = coin_get_value_cents() % 100;
    if (frac < 10) out.print('0');
    out.println(frac);
    out.println("OK");
}

// AT+EEPROM_RESET  — overwrite storage with factory defaults
static void cmd_eeprom_reset(const char* args, Stream& out) {
    (void)args;
    AppConfig defaults;
    config_defaults(&defaults);
    config_save(&defaults);
    out.println("Config reset to factory defaults");
    config_print(&defaults, out);
    out.println("OK");
}

// AT+RESET  — soft-reset the MCU so new config values take effect
static void cmd_reset(const char* args, Stream& out) {
    (void)args;
    out.println("Resetting MCU...");
    out.flush();
    // Strapping pins and the relay must be put back to a safe state first — see
    // periph_reset_safe(). The STM32 needed no equivalent step.
    periph_reset_safe();
    delay(20);
    esp_restart();
}

// AT+RELAY_MS=<ms>  — set relay ON time (100..3600000 ms)
static void cmd_relay_ms(const char* args, Stream& out) {
    if (args[0] != '=') { out.println("ERROR: usage  AT+RELAY_MS=<ms>"); return; }
    const long ms = atol(args + 1);
    if (ms < 100 || ms > 3600000) { out.println("ERROR: value must be 100..3600000"); return; }
    AppConfig cfg;
    stored_read_cfg(&cfg);
    cfg.relay_on_ms = (uint32_t)ms;
    config_save(&cfg);
    out.print("relay_on_ms = "); out.print(cfg.relay_on_ms); out.println(" ms");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+COINS_REQ=<n>  — set coins_required (1..99)
static void cmd_coins_req(const char* args, Stream& out) {
    if (args[0] != '=') { out.println("ERROR: usage  AT+COINS_REQ=<n>"); return; }
    const long n = atol(args + 1);
    if (n < 1 || n > 99) { out.println("ERROR: value must be 1..99"); return; }
    AppConfig cfg;
    stored_read_cfg(&cfg);
    cfg.coins_required = (uint32_t)n;
    config_save(&cfg);
    out.print("coins_required = "); out.println(cfg.coins_required);
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+BEEP_START=<hz>,<count>,<on_ms>
static void cmd_beep_start(const char* args, Stream& out) {
    if (args[0] != '=') {
        out.println("ERROR: usage  AT+BEEP_START=<hz>,<count>,<on_ms>");
        return;
    }
    const char* p = args + 1;
    long hz    = atol(p); while (*p && *p != ',') ++p; if (*p) ++p;
    long count = atol(p); while (*p && *p != ',') ++p; if (*p) ++p;
    long on_ms = atol(p);
    if (hz < 100 || hz > 20000 || count < 1 || count > 10 || on_ms < 10 || on_ms > 5000) {
        out.println("ERROR: hz 100..20000, count 1..10, on_ms 10..5000");
        return;
    }
    AppConfig cfg;
    stored_read_cfg(&cfg);
    cfg.beep_start_freq_hz = (uint16_t)hz;
    cfg.beep_start_count   = (uint8_t)count;
    cfg.beep_start_on_ms   = (uint16_t)on_ms;
    config_save(&cfg);
    out.print("beep_start = "); out.print(hz); out.print(" Hz x");
    out.print(count); out.print(", on="); out.print(on_ms); out.println(" ms");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+BEEP_END=<hz>,<count>,<on_ms>
static void cmd_beep_end(const char* args, Stream& out) {
    if (args[0] != '=') {
        out.println("ERROR: usage  AT+BEEP_END=<hz>,<count>,<on_ms>");
        return;
    }
    const char* p = args + 1;
    long hz    = atol(p); while (*p && *p != ',') ++p; if (*p) ++p;
    long count = atol(p); while (*p && *p != ',') ++p; if (*p) ++p;
    long on_ms = atol(p);
    if (hz < 100 || hz > 20000 || count < 1 || count > 10 || on_ms < 10 || on_ms > 5000) {
        out.println("ERROR: hz 100..20000, count 1..10, on_ms 10..5000");
        return;
    }
    AppConfig cfg;
    stored_read_cfg(&cfg);
    cfg.beep_end_freq_hz = (uint16_t)hz;
    cfg.beep_end_count   = (uint8_t)count;
    cfg.beep_end_on_ms   = (uint16_t)on_ms;
    config_save(&cfg);
    out.print("beep_end = "); out.print(hz); out.print(" Hz x");
    out.print(count); out.print(", on="); out.print(on_ms); out.println(" ms");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+BEEP_OFF_MS=<ms>  — gap between beeps
static void cmd_beep_off(const char* args, Stream& out) {
    if (args[0] != '=') { out.println("ERROR: usage  AT+BEEP_OFF_MS=<ms>"); return; }
    const long ms = atol(args + 1);
    if (ms < 10 || ms > 5000) { out.println("ERROR: value must be 10..5000"); return; }
    AppConfig cfg;
    stored_read_cfg(&cfg);
    cfg.beep_off_ms = (uint16_t)ms;
    config_save(&cfg);
    out.print("beep_off_ms = "); out.print(cfg.beep_off_ms); out.println(" ms");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+OPMODE?      — show current operation mode
// AT+OPMODE=<n>   — set operation mode: 0=PressToStart, 1=PauseResume, 3=AutoStart
// NOTE: values are NOT contiguous — 2 (ON/OFF Toggle) is reserved but unimplemented.
static void cmd_opmode(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("operation_mode = "); out.print(cfg.operation_mode);
        switch (cfg.operation_mode) {
            case OP_PRESS_TO_START: out.println(" (Press to Start)"); break;
            case OP_PAUSE_RESUME:   out.println(" (Pause/Resume)");   break;
            case OP_ON_OFF_TOGGLE:  out.println(" (ON/OFF Toggle)");  break;
            case OP_AUTO_START:     out.println(" (Auto Start)");     break;
            case OP_SENSOR_STOP:    out.println(" (reserved, unused — see AT+SENSOR_ENABLE)"); break;
            default:                out.println(" (unknown)");        break;
        }
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+OPMODE=<n>  (0=PressToStart, 1=PauseResume, 3=AutoStart)"); return; }
    const long n = atol(args + 1);
    bool implemented = false;
    for (uint8_t i = 0; i < NUM_OPERATION_MODES_IMPLEMENTED; ++i) {
        if (kImplementedOpModes[i] == n) { implemented = true; break; }
    }
    if (n < 0 || n > 255 || !implemented) {
        out.println("ERROR: only modes 0 (Press to Start), 1 (Pause/Resume), and 3 (Auto Start) are implemented today");
        return;
    }
    cfg.operation_mode = (uint8_t)n;
    config_save(&cfg);
    out.print("operation_mode = "); out.println(cfg.operation_mode);
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+PER_CREDIT?    — show whether Per Credit Mode is enabled
// AT+PER_CREDIT=<0|1> — dispense one credit block per button press instead of all at once
static void cmd_per_credit(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("press_to_start_per_credit = "); out.println(cfg.press_to_start_per_credit ? "1 (ON)" : "0 (OFF)");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+PER_CREDIT=<0|1>"); return; }
    const long n = atol(args + 1);
    if (n != 0 && n != 1) { out.println("ERROR: value must be 0 or 1"); return; }
    cfg.press_to_start_per_credit = (n != 0);
    config_save(&cfg);
    out.print("press_to_start_per_credit = "); out.println(cfg.press_to_start_per_credit ? "ON" : "OFF");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+INACTIVITY_TIMEOUT?      — show the shared inactivity timeout (seconds)
// AT+INACTIVITY_TIMEOUT=<sec> — 0=disabled(wait indefinitely), else 0..3600
// Covers both: auto-start the initial credit (OP_PRESS_TO_START/OP_PAUSE_RESUME)
// and, in Per Credit Mode, auto-advance to the next credit between presses.
static void cmd_inactivity_timeout(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("inactivity_timeout_s = "); out.print(cfg.inactivity_timeout_s);
        out.println(cfg.inactivity_timeout_s == 0 ? " (disabled)" : " s");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+INACTIVITY_TIMEOUT=<sec>  (0=disabled, 0..3600)"); return; }
    const long n = atol(args + 1);
    if (n < 0 || n > 3600) { out.println("ERROR: value must be 0..3600"); return; }
    cfg.inactivity_timeout_s = (uint16_t)n;
    config_save(&cfg);
    out.print("inactivity_timeout_s = "); out.print(cfg.inactivity_timeout_s); out.println(" s");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+ACCUMULATION?    — show whether Coin Accumulation is enabled
// AT+ACCUMULATION=<0|1> — composable with any operation_mode
static void cmd_accumulation(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("accumulation_enabled = "); out.println(cfg.accumulation_enabled ? "1 (ON)" : "0 (OFF)");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+ACCUMULATION=<0|1>"); return; }
    const long n = atol(args + 1);
    if (n != 0 && n != 1) { out.println("ERROR: value must be 0 or 1"); return; }
    cfg.accumulation_enabled = (n != 0);
    config_save(&cfg);
    out.print("accumulation_enabled = "); out.println(cfg.accumulation_enabled ? "ON" : "OFF");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+SENSOR_ENABLE?    — show whether Sensor Stop is enabled
// AT+SENSOR_ENABLE=<0|1> — composable with any operation_mode
static void cmd_sensor_enable(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("sensor_stop_enabled = "); out.println(cfg.sensor_stop_enabled ? "1 (ON)" : "0 (OFF)");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+SENSOR_ENABLE=<0|1>"); return; }
    const long n = atol(args + 1);
    if (n != 0 && n != 1) { out.println("ERROR: value must be 0 or 1"); return; }
    cfg.sensor_stop_enabled = (n != 0);
    config_save(&cfg);
    out.print("sensor_stop_enabled = "); out.println(cfg.sensor_stop_enabled ? "ON" : "OFF");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+SENSOR_POLARITY?      — show trigger polarity
// AT+SENSOR_POLARITY=<0|1> — 0=active-LOW (default), 1=active-HIGH
static void cmd_sensor_polarity(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("sensor_active_high = "); out.println(cfg.sensor_active_high ? "1 (HIGH)" : "0 (LOW)");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+SENSOR_POLARITY=<0|1>  (0=LOW,1=HIGH)"); return; }
    const long n = atol(args + 1);
    if (n != 0 && n != 1) { out.println("ERROR: value must be 0 or 1"); return; }
    cfg.sensor_active_high = (n != 0);
    config_save(&cfg);
    out.print("sensor_active_high = "); out.println(cfg.sensor_active_high ? "HIGH" : "LOW");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+SENSOR_WAIT_MS?    — show required continuous-active time before trigger
// AT+SENSOR_WAIT_MS=<ms> — 0 = immediate trigger, >0 = must stay active this long (0..5000)
static void cmd_sensor_wait_ms(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("sensor_wait_ms = "); out.print(cfg.sensor_wait_ms);
        out.println(cfg.sensor_wait_ms == 0 ? " (immediate)" : " ms");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+SENSOR_WAIT_MS=<ms>  (0=immediate)"); return; }
    const long ms = atol(args + 1);
    if (ms < 0 || ms > 5000) { out.println("ERROR: value must be 0..5000"); return; }
    cfg.sensor_wait_ms = (uint16_t)ms;
    config_save(&cfg);
    out.print("sensor_wait_ms = "); out.print(cfg.sensor_wait_ms); out.println(" ms");
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+DISPLAY_TYPE?      — show current display type
// AT+DISPLAY_TYPE=<n>   — set display type: 0=none, 1=LCD16x2(default), 2=7seg4digit
static void cmd_display_type(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("display_type = "); out.print(cfg.display_type);
        switch (cfg.display_type) {
            case DISPLAY_NONE:      out.println(" (none)");                break;
            case DISPLAY_LCD_16X2:  out.println(" (LCD 16x2 I2C)");       break;
            case DISPLAY_7SEG_4DIG: out.println(" (4-digit 7-segment)");  break;
            default:                out.println(" (unknown)");             break;
        }
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+DISPLAY_TYPE=<n>  (0=none,1=LCD16x2,2=7seg)"); return; }
    const long n = atol(args + 1);
    if (n < 0 || n > 2) { out.println("ERROR: value must be 0..2"); return; }
    cfg.display_type = (uint8_t)n;
    config_save(&cfg);
    out.print("display_type = "); out.println(cfg.display_type);
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// AT+PRICE_PER_CREDIT?    — show price per pulse, in centavos
// AT+PRICE_PER_CREDIT=<c> — set price per pulse in centavos (0..99999, up to P999.99)
static void cmd_price_per_credit(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("price_per_credit_cents = "); out.print(cfg.price_per_credit_cents);
        out.print("  (P"); out.print(cfg.price_per_credit_cents / 100); out.print('.');
        const uint32_t frac = cfg.price_per_credit_cents % 100;
        if (frac < 10) out.print('0');
        out.print(frac); out.println(")");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+PRICE_PER_CREDIT=<centavos>  (0..99999)"); return; }
    const long n = atol(args + 1);
    if (n < 0 || n > 99999) { out.println("ERROR: value must be 0..99999"); return; }
    cfg.price_per_credit_cents = (uint32_t)n;
    config_save(&cfg);
    out.print("price_per_credit_cents = "); out.println(cfg.price_per_credit_cents);
    out.println("NOTE: send AT+RESET to apply");
    out.println("OK");
}

// ============================================================================
// ESP32-only commands — no STM32 counterpart. All four exist because this board
// has hardware the STM32 did not, or unknowns the STM32 did not have.
// ============================================================================

// AT+COIN_POLARITY?      — which level counts as a coin pulse
// AT+COIN_POLARITY=<0|1> — 0=active-LOW (default), 1=active-HIGH
// The PC817 (U4) output swing is not in the schematic, and this is money — see
// AppConfig::coin_active_high and docs/PENDING.md item 3.
static void cmd_coin_polarity(const char* args, Stream& out) {
    AppConfig cfg;
    stored_read_cfg(&cfg);

    if (args[0] == '?') {
        out.print("coin_active_high = ");
        out.println(cfg.coin_active_high ? "1 (idle LOW, pulse HIGH)"
                                         : "0 (idle HIGH, pulse LOW)");
        out.print("live COIN_IN level = ");
        out.println(coin_raw_level() ? "HIGH" : "LOW");
        out.println("OK");
        return;
    }

    if (args[0] != '=') { out.println("ERROR: usage  AT+COIN_POLARITY=<0|1>  (0=pulse LOW, 1=pulse HIGH)"); return; }
    const long n = atol(args + 1);
    if (n != 0 && n != 1) { out.println("ERROR: value must be 0 or 1"); return; }
    cfg.coin_active_high = (n != 0);
    config_save(&cfg);
    // Applied live as well as stored: this one is a bench-calibration command,
    // and making the operator reboot between attempts would turn finding the
    // right polarity into a much slower loop than it needs to be.
    coin_counter_set_polarity(cfg.coin_active_high);
    out.print("coin_active_high = "); out.println(cfg.coin_active_high ? "HIGH" : "LOW");
    out.println("(applied live — no reset needed)");
    out.println("OK");
}

// AT+COIN?  — live coin input state, for bench-checking the acceptor
static void cmd_coin_status(const char* args, Stream& out) {
    (void)args;
    out.print("COIN_IN level    = "); out.println(coin_raw_level() ? "HIGH" : "LOW");
    out.print("counting polarity= "); out.println(coin_counter_polarity() ? "active HIGH" : "active LOW");
    out.print("pulses counted   = "); out.println(coin_get_count());
    out.print("value banked     = "); out.print(coin_get_value_cents()); out.println(" centavos");
    out.print("coin slot (IO12) = "); out.println(coin_slot_enabled() ? "ENABLED" : "inhibited");
    out.println("If dropping a coin moves the level but not the count, flip");
    out.println("AT+COIN_POLARITY and try again.");
    out.println("OK");
}

// AT+WDT?  — external watchdog status. See src/wdt.h for why this is worth having.
static void cmd_wdt(const char* args, Stream& out) {
    (void)args;
    wdt_print_status(out);
    out.println("OK");
}

// AT+STRAP?  — JP10 role strap + reset reason. Both are boot-time facts that are
// otherwise invisible once the banner has scrolled away.
static void cmd_strap(const char* args, Stream& out) {
    (void)args;
    out.print("JP10 I2C_CONFG (IO5) = ");
    out.print(strap_role_high() ? "HIGH" : "LOW");
    out.println(strap_role_high() ? "  (jumper OPEN, R17 pull-up)"
                                  : "  (jumper 1-2 BRIDGED)");
    out.println("  (read-only, not acted on yet — master/slave split is a later phase)");
    wdt_print_boot_report(out);
    out.println("OK");
}

// AT+LCD?  — display health, address, recovery count
static void cmd_lcd_status(const char* args, Stream& out) {
    (void)args;
    display_print_status(out);
    out.println("OK");
}

// AT+COUNTERS?  — earnings totals, i.e. exactly what BLE Live Counters (6a40f003)
// reports. The bench equivalent of the app's Diagnostics coin-path wizard: note
// `lifetime`, drop a coin, run it again. If lifetime moved, the coin path and
// the configured polarity are both correct (APP_BLE_PLAN H1/A3).
static void cmd_counters(const char* args, Stream& out) {
    (void)args;
    counters_print(out);
    out.println("OK");
}

// AT+LOG?        — event log health: depth, seq range, wraps, drops
// AT+LOG=<n>     — dump the n oldest retained events
// AT+LOG=<n>,<after_seq> — dump n events after a given seq, i.e. exactly what a
//                  BLE Session Log page would carry for that cursor
//
// The bench counterpart of the app's delta sync: `AT+LOG=10,0` is the first page
// the app would pull on a machine it has never synced, in the same order and
// with the same PESO amounts (the wire carries pesos, not centavos — D15).
static void cmd_log(const char* args, Stream& out) {
    if (args[0] == '?' || args[0] == ' ' || args[0] == '\0') {
        eventlog_print(out);
        out.println("OK");
        return;
    }
    if (args[0] == '=') {
        const char* p = args + 1;
        const long n = strtol(p, nullptr, 10);
        if (n <= 0 || n > 1000) {
            out.println("ERROR: usage  AT+LOG=<rows>[,<after_seq>]   (rows 1..1000)");
            return;
        }
        uint32_t after = 0;
        const char* comma = strchr(p, ',');
        if (comma != nullptr) after = (uint32_t)strtoul(comma + 1, nullptr, 10);
        eventlog_dump(out, after, (uint32_t)n);
        out.println("OK");
        return;
    }
    out.println("ERROR: usage  AT+LOG?  or  AT+LOG=<rows>[,<after_seq>]");
}

// AT+DIAG?       — the fault mailbox and live sensor bits, i.e. exactly what
//                  BLE Diagnostics (6a40f006) reports.
// AT+DIAG_CLEAR  — empty the fault list.
//
// Clearing normally comes from the app via the Command characteristic's
// clear_errors op, once it has uploaded the faults to the backend. f007 is not
// built yet, so this is currently the only way to empty the list — and on the
// bench it is genuinely useful, because pressing RESET raises 0x05 every time
// (SW1 and the TPL5010 share ESP_EN and cannot be told apart).
static void cmd_diag(const char* args, Stream& out) {
    if (args[0] == '?' || args[0] == '\0') {
        diag_print(out);
        out.println("OK");
        return;
    }
    out.println("ERROR: usage  AT+DIAG?  (or AT+DIAG_CLEAR to empty the fault list)");
}

static void cmd_diag_clear(const char* args, Stream& out) {
    (void)args;
    diag_clear_all();
    out.println("fault list cleared");
    out.println("NOTE: 0x03 (RTC unset) re-raises immediately if the clock is still");
    out.println("      unset — it is a present condition, not a historical event.");
    diag_print(out);
    out.println("OK");
}

// AT+RTC?                          — clock status, both chip and system
// AT+RTC=<epoch>                   — set from epoch seconds UTC (what BLE sends)
// AT+RTC=YYYY-MM-DD HH:MM:SS       — set from a human-typed UTC datetime
//
// Both forms exist for a reason: the BLE Time Sync characteristic (6a40f002)
// carries epoch_utc:u32, so epoch is the native form and scripts should use it;
// but nobody sitting at a bench can type an epoch without a converter.
//
// TIME IS UTC, always. There is no local-time setter and should not be — the
// event log and the app contract are both UTC, and a board that stored local
// time would silently produce timestamps 8 hours out.
static void cmd_rtc(const char* args, Stream& out) {
    if (args[0] == '?' || args[0] == '\0') {
        rtc_print(out);
        out.println("OK");
        return;
    }

    if (args[0] != '=') {
        out.println("ERROR: usage  AT+RTC=<epoch>  or  AT+RTC=YYYY-MM-DD HH:MM:SS  (UTC)");
        return;
    }

    const char* val = args + 1;
    while (*val == ' ') ++val;

    uint32_t epoch = 0;
    if (strchr(val, '-') != nullptr) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        const int n = sscanf(val, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
        if (n < 3) {
            out.println("ERROR: expected YYYY-MM-DD HH:MM:SS (time optional, UTC)");
            return;
        }
        if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
            h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) {
            out.println("ERROR: field out of range");
            return;
        }
        RtcTime t;
        t.year  = (uint8_t)(y - 2000);
        t.month = (uint8_t)mo;
        t.date  = (uint8_t)d;
        t.hour  = (uint8_t)h;
        t.min   = (uint8_t)mi;
        t.sec   = (uint8_t)se;
        t.dow   = 1;                    // recomputed by rtc_set_epoch
        epoch = rtc_time_to_epoch(&t);
    } else {
        epoch = (uint32_t)strtoul(val, nullptr, 10);
    }

    if (!rtc_set_epoch(epoch)) {
        out.print("ERROR: rejected — epoch must be within ");
        out.print((unsigned long)RTC_EPOCH_MIN); out.print("..");
        out.print((unsigned long)RTC_EPOCH_MAX);
        out.println(" (2026..2050), and the chip must be writable");
        out.println("  A value outside that window is almost always an uninitialised");
        out.println("  clock, and accepting it would corrupt event-log ordering.");
        return;
    }

    out.println("time set (chip + system clock, no reset needed)");
    rtc_print(out);
    out.println("OK");
}

// AT+SERIAL?                       — report the factory serial (or that there isn't one)
// AT+SERIAL=VLABS-S1-NNNNN       — set it, ONCE
//
// This is the app's `device_id`. Unlike every other setter here it does not
// edit the config blob and does not need AT+RESET — identity lives in its own
// NVS namespace (see src/identity.h for why) and is read straight from there.
//
// WRITE-ONCE, deliberately. A freely rewritable serial would let anyone with a
// cable at J7 retype board A's identity onto board B, after which the two
// boards report as one machine and their earnings merge into a single backend
// row — quietly, and in whichever direction suited whoever did it. Unlike the
// free-vend exposure in REVIEW_FINDINGS.md R22, that corrupts data rather than
// giving away a vend, so it would never show up in a till count.
// AT+SERIAL_ERASE is the RMA/refurb path.
static void cmd_serial(const char* args, Stream& out) {
    if (args[0] == '?' || args[0] == '\0') {
        identity_print(out);
        if (!identity_serial_valid()) {
            out.println("NOTE: this board has no identity — the app cannot key earnings to it.");
        }
        out.println("OK");
        return;
    }

    if (args[0] != '=') {
        out.println("ERROR: usage  AT+SERIAL=" IDENTITY_SERIAL_PATTERN "   (or AT+SERIAL? to read)");
        return;
    }

    const char* val = args + 1;
    while (*val == ' ') ++val;

    switch (identity_serial_set(val)) {
        case IDENTITY_SET_OK: {
            char serial[IDENTITY_SERIAL_BUF];
            identity_serial_get(serial);
            out.print("serial = "); out.println(serial);
            out.println("(stored and verified — no reset needed; this board is now provisioned)");
            out.println("OK");
            break;
        }
        case IDENTITY_SET_ALREADY: {
            char serial[IDENTITY_SERIAL_BUF];
            identity_serial_get(serial);
            out.print("ERROR: serial already set to "); out.println(serial);
            out.println("Write-once by design. To re-provision (RMA/refurb) use:");
            out.println("  AT+SERIAL_ERASE=<token>   — see AT+SERIAL_ERASE? for this board's token");
            break;
        }
        case IDENTITY_SET_BAD_FORMAT:
            out.println("ERROR: expected " IDENTITY_SERIAL_PATTERN
                        "  (exactly " IDENTITY_SERIAL_DIGITS_STR " digits)");
            out.println("  e.g. AT+SERIAL=" IDENTITY_SERIAL_EXAMPLE);
            break;
        case IDENTITY_SET_NVS_FAIL:
            out.println("ERROR: NVS write failed — serial NOT stored. Retry; if it persists the");
            out.println("       nvs partition may be full or damaged.");
            break;
    }
}

// AT+SERIAL_ERASE?         — show this board's erase token
// AT+SERIAL_ERASE=<token>  — clear the serial so it can be set again
//
// The token is the last 3 bytes of this board's own MAC. It is not a secret and
// is not meant to be: the point is that clearing an identity should require
// being able to read the board in your hand, so a mistyped or pasted command
// cannot orphan a different unit.
static void cmd_serial_erase(const char* args, Stream& out) {
    char token[IDENTITY_TOKEN_BUF];
    identity_erase_token(token);

    if (args[0] == '?' || args[0] == '\0') {
        out.print("erase token for THIS board = "); out.println(token);
        out.print("usage: AT+SERIAL_ERASE="); out.println(token);
        out.println("OK");
        return;
    }

    if (args[0] != '=') {
        out.println("ERROR: usage  AT+SERIAL_ERASE=<token>   (AT+SERIAL_ERASE? shows it)");
        return;
    }

    if (!identity_serial_valid()) {
        out.println("ERROR: no serial stored — nothing to erase");
        return;
    }

    const char* val = args + 1;
    while (*val == ' ') ++val;

    if (!identity_serial_erase(val)) {
        out.println("ERROR: wrong token, or the NVS erase failed — serial NOT cleared");
        out.println("       AT+SERIAL_ERASE? shows the token for the board you are connected to.");
        return;
    }

    out.println("serial erased — this board is now UNPROVISIONED");
    out.println("Set a new one with AT+SERIAL=" IDENTITY_SERIAL_PATTERN);
    out.println("OK");
}

// ============================================================================
// Command table
//
// ORDERING: dispatch() takes the first prefix match, so longer names must come
// before any shorter name they start with. AT+COIN_POLARITY and AT+COINS_REQ
// both precede AT+COIN — otherwise "AT+COIN" would swallow both. Likewise
// AT+SERIAL_ERASE precedes AT+SERIAL, or an erase would arrive at cmd_serial()
// as args="_ERASE=..." and be rejected as a bad format.
// ============================================================================
static const CliCommand kCommands[] = {
    { "AT+HELP?",        "List all supported commands",                          cmd_help        },
    { "AT+PING",         "Reply with PONG (link check)",                         cmd_ping        },
    { "AT+VERSION?",     "Report firmware version",                              cmd_version     },
    { "AT+EEPROM_RESET", "Overwrite stored config with factory defaults",        cmd_eeprom_reset},
    { "AT+EEPROM?",      "Dump raw stored bytes + all parsed config fields",     cmd_eeprom_dump },
    { "AT+CFG?",         "Show parsed config (relay time, beeps, etc.)",         cmd_cfg_show    },
    { "AT+CREDITS?",     "Show current in-RAM credit count and money total",     cmd_credits     },
    { "AT+RESET",        "Soft-reset the MCU (applies stored config changes)",   cmd_reset       },
    { "AT+RELAY_MS",     "Set relay ON time: AT+RELAY_MS=<ms>  (100..3600000)",  cmd_relay_ms    },
    { "AT+COINS_REQ",    "Set coins per credit: AT+COINS_REQ=<n>  (1..99)",      cmd_coins_req   },
    { "AT+COIN_POLARITY","Set/get coin pulse polarity: AT+COIN_POLARITY=<0|1> (0=pulse LOW)", cmd_coin_polarity },
    { "AT+COIN?",        "Live coin input state (level, polarity, pulses)",      cmd_coin_status },
    { "AT+OPMODE",       "Set/get operation mode: AT+OPMODE=<n>  (0=PressToStart, 1=PauseResume, 3=AutoStart)", cmd_opmode  },
    { "AT+PER_CREDIT",   "Set/get Per Credit Mode: AT+PER_CREDIT=<0|1>",         cmd_per_credit  },
    { "AT+INACTIVITY_TIMEOUT", "Set/get shared inactivity timeout: AT+INACTIVITY_TIMEOUT=<sec> (0=disabled, 0..3600)", cmd_inactivity_timeout },
    { "AT+BEEP_START",   "Set start beep: AT+BEEP_START=<hz>,<count>,<on_ms>",   cmd_beep_start  },
    { "AT+BEEP_END",     "Set end beep:   AT+BEEP_END=<hz>,<count>,<on_ms>",     cmd_beep_end    },
    { "AT+BEEP_OFF_MS",  "Set gap between beeps: AT+BEEP_OFF_MS=<ms>",           cmd_beep_off    },
    { "AT+DISPLAY_TYPE", "Set/get display: AT+DISPLAY_TYPE=<n>  (0=none,1=LCD,2=7seg)", cmd_display_type },
    { "AT+ACCUMULATION",    "Set/get Coin Accumulation: AT+ACCUMULATION=<0|1>",                  cmd_accumulation    },
    { "AT+SENSOR_ENABLE",   "Set/get Sensor Stop: AT+SENSOR_ENABLE=<0|1>",                       cmd_sensor_enable   },
    { "AT+SENSOR_POLARITY", "Set/get trigger polarity: AT+SENSOR_POLARITY=<0|1> (0=LOW,1=HIGH)", cmd_sensor_polarity },
    { "AT+SENSOR_WAIT_MS",  "Set required active time before trigger: AT+SENSOR_WAIT_MS=<ms> (0=immediate, 0..5000)", cmd_sensor_wait_ms },
    { "AT+PRICE_PER_CREDIT", "Set/get price per pulse: AT+PRICE_PER_CREDIT=<centavos> (0..99999)", cmd_price_per_credit },
    { "AT+WDT?",         "External TPL5010 watchdog status + liveness ages",     cmd_wdt         },
    { "AT+STRAP?",       "JP10 role strap position + last reset reason",         cmd_strap       },
    { "AT+LCD?",         "LCD health, I2C address, recovery count",              cmd_lcd_status  },
    // AT+DIAG_CLEAR before AT+DIAG — dispatch() takes the first prefix match.
    { "AT+DIAG_CLEAR",    "Empty the diagnostics fault list (app normally does this via f007)", cmd_diag_clear },
    { "AT+DIAG",          "Fault list + live sensor bits (same data as BLE Diagnostics 6a40f006)", cmd_diag },
    { "AT+COUNTERS?",     "Earnings totals (same data as BLE Live Counters 6a40f003)", cmd_counters },
    { "AT+LOG",           "Event log: AT+LOG? or AT+LOG=<rows>[,<after_seq>] (same data as BLE Session Log 6a40f004)", cmd_log },
    { "AT+RTC",          "Clock: AT+RTC? or AT+RTC=<epoch> or AT+RTC=YYYY-MM-DD HH:MM:SS (UTC)", cmd_rtc },
    // AT+SERIAL_ERASE must precede AT+SERIAL — see the ORDERING note above.
    { "AT+SERIAL_ERASE", "Clear the serial for re-provisioning: AT+SERIAL_ERASE=<token> (AT+SERIAL_ERASE? shows it)", cmd_serial_erase },
    { "AT+SERIAL",       "Board identity (app device_id): AT+SERIAL? or AT+SERIAL=" IDENTITY_SERIAL_PATTERN " (write-once)", cmd_serial },
};
static const size_t kNumCommands = sizeof(kCommands) / sizeof(kCommands[0]);

const CliCommand* cli_get_commands(size_t& count) {
    count = kNumCommands;
    return kCommands;
}

// ============================================================================
// Parser — identical in structure to the STM32 firmware and the test harness
// ============================================================================

#define CLI_LINE_MAX 96
static char   s_buf[CLI_LINE_MAX];
static size_t s_len = 0;

static const char* match_command(const char* line, const char* name) {
    const size_t nlen = strlen(name);
    for (size_t i = 0; i < nlen; ++i) {
        if (line[i] == '\0') return nullptr;
        if (toupper((unsigned char)line[i]) != toupper((unsigned char)name[i])) return nullptr;
    }
    return line + nlen;
}

static void dispatch(char* line, Stream& out) {
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0') return;

    for (size_t i = 0; i < kNumCommands; ++i) {
        const char* args = match_command(line, kCommands[i].name);
        if (args) {
            kCommands[i].handler(args, out);
            return;
        }
    }

    out.print("ERROR: unknown command '");
    out.print(line);
    out.println("'  — try AT+HELP?");
}

void cli_feed_char(char c, Stream& out) {
    if (c == '\r' || c == '\n') {
        if (s_len > 0) {
            s_buf[s_len] = '\0';
            dispatch(s_buf, out);
            s_len = 0;
        }
        return;
    }
    if (c == 8 || c == 127) {   // backspace / DEL
        if (s_len > 0) --s_len;
        return;
    }
    if (s_len < CLI_LINE_MAX - 1) {
        s_buf[s_len++] = c;
    } else {
        s_len = 0;
        out.println("ERROR: line too long");
    }
}
