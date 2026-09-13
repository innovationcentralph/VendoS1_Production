#include "ble_deviceinfo.h"

#ifdef ENABLE_BLE_DEVICE_INFO

#include "identity.h"
#include "config.h"
#include "version.h"
#include <Arduino.h>
#include <string.h>

#define BLE_DEVICEINFO_CHAR_UUID "6a40f001-0000-1000-8000-00805f9b0001"

// Human-facing product name. Fixed for this board; the app only displays it.
#define BLE_DEVICEINFO_MODEL "Vendo S1"

static NimBLECharacteristic* s_char = nullptr;

// Appends a length-prefixed UTF-8 string. Silently truncates at 255 because the
// prefix is a u8 — none of the three fields comes close, but a truncated string
// is recoverable where an overflowed length byte would desynchronise every field
// after it and hand the app garbage it would parse confidently.
static size_t put_str(uint8_t* buf, size_t off, const char* s) {
    size_t len = strlen(s);
    if (len > 255) len = 255;
    buf[off++] = (uint8_t)len;
    memcpy(buf + off, s, len);
    return off + len;
}

// Which optional hardware this board reports. Display-only in the app — it
// renders a badge row on MachineDetailScreen — so the cost of getting one wrong
// is a misleading label, not broken behaviour.
static uint8_t modules_bitmap() {
    AppConfig cfg;
    if (!config_load_quiet(&cfg)) config_defaults(&cfg);

    uint8_t b = 0;

    // Always present: BZ1 is fitted on every S1 and is not optional.
    b |= (uint8_t)(1u << BLE_MODULE_BIT_BUZZER);

    // The contract models door and vibration as separate sensors. The firmware
    // has neither — it has ONE generic sensor input (PIN_SENSOR_IN, J6 AUX_1),
    // shared by whatever the installer wired to it, driving the single
    // sensor_stop_enabled flag. See docs/BLE_CONFIG_CONTRACT.md.
    //
    // So this reports `door` as the app's generic-sensor slot when Sensor Stop
    // is configured, and never claims `vibration`. If the installed sensor is
    // actually a vibration switch the badge reads slightly wrong; claiming two
    // independent sensors that do not exist would be worse.
    if (cfg.sensor_stop_enabled) b |= (uint8_t)(1u << BLE_MODULE_BIT_DOOR);

    // GSM is deliberately NEVER reported, even when gsm_reporting_enabled is
    // set. The flag is honoured as config on both platforms but there is no
    // gsm_report_send() on either — it is a stub. Reporting the module as
    // present would put a badge on a machine for a feature that does nothing,
    // and the first support call about "why is GSM not reporting" would be
    // firmware's fault. See docs/PENDING.md item 8.

    return b;
}

static void republish() {
    if (s_char == nullptr) return;

    char serial[IDENTITY_SERIAL_BUF];
    char unset[24];
    const char* device_id;

    if (identity_serial_get(serial)) {
        device_id = serial;
    } else {
        // An unprovisioned board reports the same string it advertises, rather
        // than an empty device_id. Two reasons: a human debugging sees one
        // identical token in the scanner and in the app, and a non-empty value
        // cannot be mistaken for a parse failure. It will not match any claimed
        // machine — which is correct, an unprovisioned board should not sync.
        uint8_t mac[6];
        identity_mac(mac);
        snprintf(unset, sizeof(unset), "VLABS-UNSET-%02X%02X", mac[4], mac[5]);
        device_id = unset;
    }

    uint8_t buf[128];
    size_t n = 0;
    buf[n++] = BLE_DEVICEINFO_SCHEMA_VERSION;
    buf[n++] = modules_bitmap();
    n = put_str(buf, n, device_id);
    n = put_str(buf, n, BLE_DEVICEINFO_MODEL);
    n = put_str(buf, n, FW_VERSION_STRING);

    s_char->setValue(buf, n);
}

class DeviceInfoCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        (void)c;
        // Rebuilt per read rather than cached at boot: the serial can be set
        // with AT+SERIAL while running, and sensor_stop_enabled can change via
        // the Config characteristic or the LCD menu. A stale read here would
        // have the app believing a board it just provisioned is still
        // unprovisioned.
        republish();
    }
};

void ble_deviceinfo_register(NimBLEService* service) {
    s_char = service->createCharacteristic(
        BLE_DEVICEINFO_CHAR_UUID,
        NIMBLE_PROPERTY::READ);
    s_char->setCallbacks(new DeviceInfoCallbacks());
    republish();    // seed a real value before the first connection

    Serial.println("BLE: Device Info (f001) ENABLED — board will present as 'full'");
    Serial.println("     to the app, which then requires f004 Session Log and f006");
    Serial.println("     Diagnostics or the whole connect fails. See src/ble_deviceinfo.h.");
}

#else  // !ENABLE_BLE_DEVICE_INFO

void ble_deviceinfo_register(NimBLEService* service) {
    (void)service;   // not built — see the header for why this is the default
}

#endif
