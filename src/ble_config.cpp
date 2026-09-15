#include "ble_config.h"
#include "config.h"
#include "identity.h"
#include "ble_timesync.h"
#include "ble_livecounters.h"
#include "ble_deviceinfo.h"
#include "ble_diagnostics.h"
#include "ble_sessionlog.h"
#include "ble_command.h"
#include "periph.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <stdio.h>

// =============================================================================
// UUIDs
//
// =============================================================================
#define BLE_SERVICE_UUID      "6a400001-0000-1000-8000-00805f9b0001"
#define BLE_CONFIG_CHAR_UUID  "6a40f005-0000-1000-8000-00805f9b0001"  // from the contract

// Advertised name.
//
// A provisioned board advertises its serial VERBATIM — "VLABS-S1-00001". Name,
// device_id and the number on the sticker are then the same string, so a board
// can be identified in a scanner list without connecting to it.
//
// This replaced the old fixed "VendoS1" (user's decision, 2026-09-13: "App will
// adapt"). It is a BREAKING CHANGE for any client that discovered boards by
// matching that name — see docs/APP_BLE_PLAN.md D1/A5. Clients should discover
// by BLE_SERVICE_UUID, which is what it is for.
//
// An UNPROVISIONED board advertises "VLABS-UNSET-<MAC last 2 bytes>" instead.
// The UNSET token is deliberately not a unit number: a board that missed the
// factory step must be obviously distinguishable in a scanner list, not merely
// a plausible-looking serial nobody recognizes.
#define BLE_DEVICE_NAME_MAX 24

static char s_device_name[BLE_DEVICE_NAME_MAX];

static void build_device_name() {
    char serial[IDENTITY_SERIAL_BUF];
    if (identity_serial_get(serial)) {
        snprintf(s_device_name, sizeof(s_device_name), "%s", serial);
    } else {
        uint8_t mac[6];
        identity_mac(mac);
        snprintf(s_device_name, sizeof(s_device_name), "VLABS-UNSET-%02X%02X",
                 mac[4], mac[5]);
    }
}

// Wire format from docs/BLE_CONFIG_CONTRACT.md: 37 bytes, little-endian,
// fixed-width. config_version is a BLE-facing schema guard only — it has no
// AppConfig counterpart and is never persisted.
#define CFG_WIRE_LEN      37
#define CFG_WIRE_VERSION  1

static NimBLECharacteristic* s_configChar = nullptr;

// Same "read stored config, or fall back to defaults" shape cli.cpp keeps as
// a local static helper — three lines, duplicated rather than shared, same
// call as cli.cpp's stored_read_cfg().
static void stored_read_cfg(AppConfig* cfg) {
    if (!config_load_quiet(cfg)) {
        config_defaults(cfg);
    }
}

static void put_u16(uint8_t* buf, size_t off, uint16_t v) {
    buf[off]     = (uint8_t)(v & 0xFF);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}
static void put_u32(uint8_t* buf, size_t off, uint32_t v) {
    buf[off]     = (uint8_t)(v & 0xFF);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t get_u16(const uint8_t* buf, size_t off) {
    return (uint16_t)(buf[off] | (buf[off + 1] << 8));
}
static uint32_t get_u32(const uint8_t* buf, size_t off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

// Packs *cfg into the 37-byte wire format from docs/BLE_CONFIG_CONTRACT.md.
static void serialize_cfg(const AppConfig* cfg, uint8_t out[CFG_WIRE_LEN]) {
    out[0] = CFG_WIRE_VERSION;
    put_u32(out, 1, cfg->relay_on_ms);
    put_u32(out, 5, cfg->coins_required);
    out[9]  = cfg->operation_mode;
    out[10] = cfg->press_to_start_per_credit ? 1 : 0;
    put_u16(out, 11, cfg->inactivity_timeout_s);
    out[13] = cfg->accumulation_enabled ? 1 : 0;
    out[14] = cfg->sensor_stop_enabled ? 1 : 0;
    out[15] = cfg->sensor_active_high ? 1 : 0;
    put_u16(out, 16, cfg->sensor_wait_ms);
    out[18] = cfg->gsm_reporting_enabled ? 1 : 0;
    out[19] = cfg->display_type;
    put_u16(out, 20, cfg->beep_start_freq_hz);
    out[22] = cfg->beep_start_count;
    put_u16(out, 23, cfg->beep_start_on_ms);
    put_u16(out, 25, cfg->beep_end_freq_hz);
    out[27] = cfg->beep_end_count;
    put_u16(out, 28, cfg->beep_end_on_ms);
    put_u16(out, 30, cfg->beep_off_ms);
    put_u32(out, 32, cfg->price_per_credit_cents);
    out[36] = cfg->coin_active_high ? 1 : 0;
}

// Validates a 37-byte wire payload and, only if every field passes, writes it
// into *cfg (which must already hold the current stored config — same
// load-then-mutate shape every CLI setter in cli.cpp uses). Returns false and
// leaves *cfg untouched if anything is wrong; logs the specific reason to
// Serial since a plain GATT write has no channel back to the client for a
// human-readable error the way the CLI's Stream& does.
//
// Bounds mirror docs/BLE_CONFIG_CONTRACT.md, which for relay_on_ms is the
// LCD's stricter 1000 ms floor rather than the CLI's looser 100 ms one (see
// that doc's open question #3 — until validation is centralized, BLE uses
// the tighter of the two).
static bool deserialize_cfg(const uint8_t* in, size_t len, AppConfig* cfg) {
    if (len != CFG_WIRE_LEN) {
        Serial.print("BLE config write rejected: expected "); Serial.print(CFG_WIRE_LEN);
        Serial.print(" bytes, got "); Serial.println((unsigned)len);
        return false;
    }
    if (in[0] != CFG_WIRE_VERSION) {
        Serial.print("BLE config write rejected: config_version "); Serial.print(in[0]);
        Serial.print(" != "); Serial.println(CFG_WIRE_VERSION);
        return false;
    }

    const uint32_t relay_on_ms            = get_u32(in, 1);
    const uint32_t coins_required         = get_u32(in, 5);
    const uint8_t  operation_mode         = in[9];
    const uint16_t inactivity_timeout_s   = get_u16(in, 11);
    const uint16_t sensor_wait_ms         = get_u16(in, 16);
    const uint8_t  display_type           = in[19];
    const uint16_t beep_start_freq_hz     = get_u16(in, 20);
    const uint8_t  beep_start_count       = in[22];
    const uint16_t beep_start_on_ms       = get_u16(in, 23);
    const uint16_t beep_end_freq_hz       = get_u16(in, 25);
    const uint8_t  beep_end_count         = in[27];
    const uint16_t beep_end_on_ms         = get_u16(in, 28);
    const uint16_t beep_off_ms            = get_u16(in, 30);
    const uint32_t price_per_credit_cents = get_u32(in, 32);

    bool op_mode_ok = false;
    for (uint8_t i = 0; i < NUM_OPERATION_MODES_IMPLEMENTED; ++i) {
        if (kImplementedOpModes[i] == operation_mode) { op_mode_ok = true; break; }
    }

    if (relay_on_ms < 1000 || relay_on_ms > 3600000) {
        Serial.println("BLE config write rejected: relay_on_ms out of range (1000..3600000)"); return false;
    }
    if (coins_required < 1 || coins_required > 99) {
        Serial.println("BLE config write rejected: coins_required out of range (1..99)"); return false;
    }
    if (!op_mode_ok) {
        Serial.println("BLE config write rejected: operation_mode not implemented (must be 0, 1, or 3)"); return false;
    }
    if (inactivity_timeout_s > 3600) {
        Serial.println("BLE config write rejected: inactivity_timeout_s out of range (0..3600)"); return false;
    }
    if (sensor_wait_ms > 5000) {
        Serial.println("BLE config write rejected: sensor_wait_ms out of range (0..5000)"); return false;
    }
    if (display_type > 2) {
        Serial.println("BLE config write rejected: display_type out of range (0..2)"); return false;
    }
    if (beep_start_freq_hz < 100 || beep_start_freq_hz > 20000) {
        Serial.println("BLE config write rejected: beep_start_freq_hz out of range (100..20000)"); return false;
    }
    if (beep_start_count < 1 || beep_start_count > 10) {
        Serial.println("BLE config write rejected: beep_start_count out of range (1..10)"); return false;
    }
    if (beep_start_on_ms < 10 || beep_start_on_ms > 5000) {
        Serial.println("BLE config write rejected: beep_start_on_ms out of range (10..5000)"); return false;
    }
    if (beep_end_freq_hz < 100 || beep_end_freq_hz > 20000) {
        Serial.println("BLE config write rejected: beep_end_freq_hz out of range (100..20000)"); return false;
    }
    if (beep_end_count < 1 || beep_end_count > 10) {
        Serial.println("BLE config write rejected: beep_end_count out of range (1..10)"); return false;
    }
    if (beep_end_on_ms < 10 || beep_end_on_ms > 5000) {
        Serial.println("BLE config write rejected: beep_end_on_ms out of range (10..5000)"); return false;
    }
    if (beep_off_ms < 10 || beep_off_ms > 5000) {
        Serial.println("BLE config write rejected: beep_off_ms out of range (10..5000)"); return false;
    }
    if (price_per_credit_cents > 99999) {
        Serial.println("BLE config write rejected: price_per_credit_cents out of range (0..99999)"); return false;
    }

    cfg->relay_on_ms              = relay_on_ms;
    cfg->coins_required           = coins_required;
    cfg->operation_mode           = operation_mode;
    cfg->press_to_start_per_credit = (in[10] != 0);
    cfg->inactivity_timeout_s     = inactivity_timeout_s;
    cfg->accumulation_enabled     = (in[13] != 0);
    cfg->sensor_stop_enabled      = (in[14] != 0);
    cfg->sensor_active_high       = (in[15] != 0);
    cfg->sensor_wait_ms           = sensor_wait_ms;
    cfg->gsm_reporting_enabled    = (in[18] != 0);
    cfg->display_type             = display_type;
    cfg->beep_start_freq_hz       = beep_start_freq_hz;
    cfg->beep_start_count         = beep_start_count;
    cfg->beep_start_on_ms         = beep_start_on_ms;
    cfg->beep_end_freq_hz         = beep_end_freq_hz;
    cfg->beep_end_count           = beep_end_count;
    cfg->beep_end_on_ms           = beep_end_on_ms;
    cfg->beep_off_ms              = beep_off_ms;
    cfg->price_per_credit_cents   = price_per_credit_cents;
    cfg->coin_active_high         = (in[36] != 0);
    return true;
}

// Re-reads the stored config and republishes it as the characteristic's
// cached value, so the next read (or this same write's response) reflects
// what is actually in NVS rather than whatever bytes the client last sent —
// including after a rejected write, where those bytes must not be echoed
// back as if they had taken effect.
static void republish_stored_cfg() {
    AppConfig cfg;
    stored_read_cfg(&cfg);
    uint8_t buf[CFG_WIRE_LEN];
    serialize_cfg(&cfg, buf);
    s_configChar->setValue(buf, CFG_WIRE_LEN);
}

class ConfigCharCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        (void)c;
        republish_stored_cfg();
    }

    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        AppConfig cfg;
        stored_read_cfg(&cfg);

        if (deserialize_cfg((const uint8_t*)v.data(), v.length(), &cfg)) {
            config_save(&cfg);
            // Same live-apply exception the CLI makes for this one field (see
            // AT+COIN_POLARITY in cli.cpp) — it's a bench-calibration value,
            // and money-critical enough that waiting for a reset to find out
            // whether a guess was right would slow that loop down badly.
            //
            // Every OTHER field here follows the CLI's persist-now,
            // apply-on-AT+RESET convention rather than the LCD's live-apply,
            // because reaching into the running app task's AppConfig from a
            // BLE callback (a different task context) would need a
            // synchronization path that does not exist yet in app.cpp — see
            // docs/BLE_CONFIG_CONTRACT.md open question #1.
            coin_counter_set_polarity(cfg.coin_active_high);
            Serial.println("BLE config write OK — stored. Send AT+RESET or power-cycle to apply the rest.");
        }
        // Invalid writes are logged (with the specific reason) inside
        // deserialize_cfg() and otherwise ignored — nothing is persisted.

        // Either way, what a follow-up read sees must reflect the real
        // stored config, not the wire bytes the client just sent.
        republish_stored_cfg();
    }
};

// Server-level callbacks. The only thing this firmware needs from them is the
// disconnect edge: Session Log keeps a pagination cursor per connection, and a
// connection handle is reused by the stack, so a slot that outlived its
// connection would hand the next client someone else's position in the log.
//
// Advertising still restarts on its own — NimBLEServer does that from
// m_advertiseOnDisconnect regardless of whether callbacks are installed — so
// this does not change the connect behaviour that already works.
class ServerCallbacks : public NimBLEServerCallbacks {
    void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
        (void)s;
        ble_sessionlog_on_disconnect(desc->conn_handle);
    }
};

void ble_config_init() {
    build_device_name();
    NimBLEDevice::init(s_device_name);
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    NimBLEService* service = server->createService(BLE_SERVICE_UUID);

    s_configChar = service->createCharacteristic(
        BLE_CONFIG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    s_configChar->setCallbacks(new ConfigCharCallbacks());
    republish_stored_cfg();  // seed a real value before the first connection

    // Other characteristics on the same service register themselves here,
    // before start(). Each lives in its own file — see src/ble_timesync.h for
    // why, and for the constraint that f001 Device Info must not ship alone.
    ble_deviceinfo_register(service);   // no-op unless ENABLE_BLE_DEVICE_INFO
    ble_timesync_register(service);
    ble_livecounters_register(service);
    ble_sessionlog_register(service);
    ble_diagnostics_register(service);
    ble_command_register(service);

    service->start();

    // Advertisement and scan response are built explicitly rather than letting
    // NimBLE place the name automatically, because the two do not both fit in
    // one packet:
    //
    //   adv payload = 31 B max
    //     flags                       3 B
    //     complete 128-bit service   18 B   <- the UUID is the expensive part
    //                               ----
    //                                21 B, leaving 10 B => ~8 chars of name
    //
    // The old fixed "VendoS1" (7 chars) squeaked in, which is why the original
    // code worked. "VLABS-S1-00001" (14) does not, and an over-long name is
    // silently truncated or drops the advertisement entirely. So the UUID stays
    // in the advertisement -- clients should discover by UUID, and it must be
    // there for them to do so -- and the name moves to the scan response, which
    // is a second 31-byte budget. Scanners request it automatically.
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();

    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setCompleteServices(NimBLEUUID(BLE_SERVICE_UUID));
    advertising->setAdvertisementData(advData);

    NimBLEAdvertisementData scanData;
    scanData.setName(s_device_name);
    advertising->setScanResponseData(scanData);

    advertising->setScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.print("BLE advertising as \""); Serial.print(s_device_name);
    Serial.println("\" — see docs/BLE_CONFIG_CONTRACT.md to test with nRF Connect");
    if (!identity_serial_valid()) {
        Serial.println("  (name suffix is the MAC — this board is UNPROVISIONED, see AT+SERIAL?)");
    }
}
