#include "ble_sessionlog.h"
#include "eventlog.h"
#include <Arduino.h>
#include <string.h>

#define BLE_SESSIONLOG_CHAR_UUID "6a40f004-0000-1000-8000-00805f9b0001"

// One per possible connection (CONFIG_BT_NIMBLE_MAX_CONNECTIONS is 3). Slots
// are claimed on first use by a connection and released on its disconnect.
#define SESSIONLOG_MAX_CURSORS 3

struct CursorSlot {
    bool     in_use;
    uint16_t conn;
    uint32_t cursor;
};

static NimBLECharacteristic* s_char = nullptr;
static CursorSlot            s_slots[SESSIONLOG_MAX_CURSORS] = {};

// Returns the slot for `conn`, claiming a free one if this is a new connection.
// If every slot is taken (more connections than we budgeted for), the last one
// is reused: a shared cursor is a degraded sync, not a corrupt one, because the
// app rewrites the cursor before every single read.
static CursorSlot* slot_for(uint16_t conn) {
    for (int i = 0; i < SESSIONLOG_MAX_CURSORS; ++i)
        if (s_slots[i].in_use && s_slots[i].conn == conn) return &s_slots[i];
    for (int i = 0; i < SESSIONLOG_MAX_CURSORS; ++i)
        if (!s_slots[i].in_use) {
            s_slots[i].in_use = true;
            s_slots[i].conn   = conn;
            s_slots[i].cursor = 0;
            return &s_slots[i];
        }
    return &s_slots[SESSIONLOG_MAX_CURSORS - 1];
}

// Serialises the page for `cursor` into the characteristic's value.
static void publish_page(uint32_t cursor) {
    uint8_t buf[EVENTLOG_PAGE_MAX_LEN];
    const size_t len = eventlog_build_page(cursor, buf);
    s_char->setValue(buf, len);
}

class SessionLogCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
        std::string v = c->getValue();
        if (v.length() < 4) {
            // Not a cursor. Ignore it rather than guessing — a short write here
            // would otherwise reset someone's pagination to 0 and re-upload
            // their whole history.
            Serial.println("BLE session log: write ignored, expected 4 bytes (after_seq:u32)");
            return;
        }
        const uint8_t* b = (const uint8_t*)v.data();
        const uint32_t cursor = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

        CursorSlot* slot = slot_for(desc->conn_handle);
        slot->cursor = cursor;

        // Build the page HERE, at the write, not at the read. The app's
        // sequence is always write-then-read, and building now means the bytes
        // a long read collects across several ATT segments were all computed
        // from one snapshot — a coin landing mid-read cannot change the page's
        // length underneath the client.
        publish_page(cursor);
    }

    void onRead(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
        (void)c;
        // A client that reads without writing first (nRF Connect, or the app's
        // very first probe) gets the page for whatever cursor that connection
        // last set — 0, i.e. from the oldest retained event, for a fresh one.
        publish_page(slot_for(desc->conn_handle)->cursor);
    }
};

void ble_sessionlog_register(NimBLEService* service) {
    s_char = service->createCharacteristic(
        BLE_SESSIONLOG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
        EVENTLOG_PAGE_MAX_LEN);
    s_char->setCallbacks(new SessionLogCallbacks());
    publish_page(0);    // seed a real value before the first connection
}

void ble_sessionlog_on_disconnect(uint16_t conn_handle) {
    for (int i = 0; i < SESSIONLOG_MAX_CURSORS; ++i)
        if (s_slots[i].in_use && s_slots[i].conn == conn_handle) {
            s_slots[i].in_use = false;
            s_slots[i].cursor = 0;
        }
}
