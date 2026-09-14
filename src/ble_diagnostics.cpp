#include "ble_diagnostics.h"
#include "diag.h"
#include <Arduino.h>

#define BLE_DIAGNOSTICS_CHAR_UUID "6a40f006-0000-1000-8000-00805f9b0001"

static NimBLECharacteristic* s_char = nullptr;

static void republish() {
    if (s_char == nullptr) return;
    uint8_t buf[DIAG_WIRE_MAX];
    const size_t n = diag_serialize(buf, sizeof(buf));
    s_char->setValue(buf, n);
}

class DiagnosticsCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        (void)c;
        // Rebuilt per read. uptime and the sensor bits are live by definition,
        // and a cached frame would show a technician a stale coin line — the
        // exact thing the Diagnostics wizard exists to watch move.
        republish();
    }
};

void ble_diagnostics_register(NimBLEService* service) {
    s_char = service->createCharacteristic(
        BLE_DIAGNOSTICS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    s_char->setCallbacks(new DiagnosticsCallbacks());
    republish();    // seed a real value before the first connection
}

void ble_diagnostics_service() {
    if (s_char == nullptr) return;

    // Notify only on a real change — a newly raised or cleared fault. NOT on
    // every uptime tick: uptime changes once a second forever, and pushing a
    // frame for it would turn an idle connection into a steady notification
    // stream for no information. A client that wants current uptime reads.
    if (!diag_take_dirty()) return;

    republish();
    s_char->notify();
}
