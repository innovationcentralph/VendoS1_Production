#include "ble_livecounters.h"
#include "counters.h"
#include <Arduino.h>

#define BLE_LIVECOUNTERS_CHAR_UUID "6a40f003-0000-1000-8000-00805f9b0001"

static NimBLECharacteristic* s_char = nullptr;

// Refreshes the characteristic's cached value from the counters module.
static void republish() {
    if (s_char == nullptr) return;
    uint8_t buf[COUNTERS_WIRE_LEN];
    counters_serialize(buf);
    s_char->setValue(buf, COUNTERS_WIRE_LEN);
}

class LiveCountersCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        (void)c;
        // Re-serialise on every read rather than serving whatever the last
        // notification happened to leave cached — same rule the Config
        // characteristic follows. A read must reflect the counters now.
        republish();
    }
};

void ble_livecounters_register(NimBLEService* service) {
    s_char = service->createCharacteristic(
        BLE_LIVECOUNTERS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    s_char->setCallbacks(new LiveCountersCallbacks());
    republish();    // seed a real value before the first connection
}

void ble_livecounters_service() {
    if (s_char == nullptr) return;

    // Driven by the counters' dirty flag, not a timer: an idle board sends
    // nothing at all, and a coin produces exactly one frame. Polling on a timer
    // would either lag the coin test or spam a connection that has no news.
    if (!counters_take_dirty()) return;

    republish();
    // No-op when nobody has subscribed, so this is safe to call unconditionally
    // — NimBLE checks the CCCD itself.
    s_char->notify();
}
