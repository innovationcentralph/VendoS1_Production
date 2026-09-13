#include "ble_timesync.h"
#include "rtc.h"
#include <Arduino.h>

// From the app team's allocation table (docs/BLE_CONFIG_CONTRACT.md). Same
// 128-bit base as the service and the Config characteristic.
#define BLE_TIMESYNC_CHAR_UUID "6a40f002-0000-1000-8000-00805f9b0001"

// epoch_utc:u32, little-endian. Fixed width, same convention as the 37-byte
// Config frame.
#define TIMESYNC_WIRE_LEN 4

static uint32_t get_u32(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

class TimeSyncCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();

        if (v.length() != TIMESYNC_WIRE_LEN) {
            Serial.print("BLE time sync rejected: expected ");
            Serial.print(TIMESYNC_WIRE_LEN);
            Serial.print(" bytes, got ");
            Serial.println((unsigned)v.length());
            return;
        }

        const uint32_t epoch = get_u32((const uint8_t*)v.data());

        // rtc_set_epoch() owns the clamp, deliberately. Validating here as well
        // would be a second copy of the rule that could drift from the CLI's —
        // exactly the divergence REVIEW_FINDINGS R18 and APP_BLE_PLAN X1
        // complain about for config bounds. One writer, one rule.
        if (!rtc_set_epoch(epoch)) {
            Serial.print("BLE time sync rejected: epoch ");
            Serial.print((unsigned long)epoch);
            Serial.print(" outside ");
            Serial.print((unsigned long)RTC_EPOCH_MIN);
            Serial.print("..");
            Serial.print((unsigned long)RTC_EPOCH_MAX);
            Serial.println(" (2026..2050), or the RTC did not accept the write");
            Serial.println("  A value outside that window is almost always an uninitialised");
            Serial.println("  clock; accepting it would corrupt event-log ordering permanently.");
            return;
        }

        Serial.print("BLE time sync OK — epoch ");
        Serial.print((unsigned long)epoch);
        Serial.println(" written to RTC + system clock (applied live, no reset)");
    }
};

void ble_timesync_register(NimBLEService* service) {
    NimBLECharacteristic* ch = service->createCharacteristic(
        BLE_TIMESYNC_CHAR_UUID,
        // WRITE only, matching the contract. A READ would be handy for
        // nRF-Connect-only verification, but the app enumerates properties and
        // the contract says Write — so read-back stays on AT+RTC? rather than
        // diverging from a spec both sides just agreed.
        NIMBLE_PROPERTY::WRITE);
    ch->setCallbacks(new TimeSyncCallbacks());
}
