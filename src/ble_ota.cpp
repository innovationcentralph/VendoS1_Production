#include "ble_ota.h"

#include "app.h"
#include "periph.h"
#include "version.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_system.h>

#define BLE_OTA_CONTROL_CHAR_UUID "6a40f008-0000-1000-8000-00805f9b0001"
#define BLE_OTA_DATA_CHAR_UUID    "6a40f009-0000-1000-8000-00805f9b0001"
#define BLE_OTA_STATUS_CHAR_UUID  "6a40f00a-0000-1000-8000-00805f9b0001"

// Op codes and status values are the app team's — Board-Firmware-Contract.md.
// This is a different op-code space from Command (f007); same numbers, other
// characteristic, no relation.
#define OTA_OP_BEGIN   0x01
#define OTA_OP_COMMIT  0x02
#define OTA_OP_ABORT   0x03

#define OTA_STATE_IDLE       0
#define OTA_STATE_RECEIVING  1
#define OTA_STATE_VERIFYING  2
#define OTA_STATE_DONE       3
#define OTA_STATE_ERROR      4

#define OTA_ERR_NONE           0
#define OTA_ERR_SIZE_MISMATCH  1
#define OTA_ERR_CRC_MISMATCH   2
#define OTA_ERR_BEGIN_FAILED   3
#define OTA_ERR_WRITE_FAILED   4
#define OTA_ERR_END_FAILED     5
// 6/7 are reserved by the contract for the WiFi path (connect/HTTP failures).
// Not used yet; listed so nothing else claims them.
#define OTA_ERR_REFUSED_BUSY   8

// An image smaller than this is not a firmware image. Catches a truncated
// upload or a wrong file before the slot is erased, which matters because the
// erase is the destructive, irreversible half of the operation.
#define OTA_MIN_IMAGE_BYTES  (64u * 1024u)

static NimBLECharacteristic* s_statusChar = nullptr;

static uint8_t  s_state          = OTA_STATE_IDLE;
static uint8_t  s_error          = OTA_ERR_NONE;
static uint32_t s_bytesReceived  = 0;
static uint32_t s_expectedSize   = 0;
static uint32_t s_expectedCrc32  = 0;
static uint32_t s_crcState       = 0xFFFFFFFF;

// Mailbox: what Control asked for, for loop() to carry out. See ble_ota.h for
// why the slow operations are not done in the GATT callback.
static volatile uint8_t  s_pendingOp    = 0;
static volatile uint32_t s_pendingSize  = 0;
static volatile uint32_t s_pendingCrc32 = 0;

// Set whenever state/error/progress changed and the app has not been told yet.
static bool s_statusDirty = false;

// Progress notifies are rate-limited: at a few hundred bytes per chunk a notify
// per chunk is thousands of notifications for one update, all carrying the same
// three fields, and they compete with the transfer for airtime.
#define OTA_PROGRESS_NOTIFY_MS 500
static uint32_t s_lastProgressMs = 0;

// -----------------------------------------------------------------------------
// CRC-32 (IEEE 802.3 / zlib / zip) — reflected poly 0xEDB88320, init/final-XOR
// 0xFFFFFFFF. mobile/src/ble/codec.ts runs the identical algorithm over the
// binary before the transfer starts, so both ends agree on what "the file's
// checksum" is without either needing a crypto library.
//
// This is a transfer check, NOT a signature: it catches corruption, not
// tampering. Anything that can write to these characteristics can flash this
// board. Signing is a separate piece of work and is required before boards we
// do not physically hold can be updated.
//
// s_crcState holds the running, un-finalised state across chunks; the final
// inversion happens once, at commit.
// -----------------------------------------------------------------------------
static void crc32_reset() { s_crcState = 0xFFFFFFFF; }

static void crc32_feed(const uint8_t* data, size_t len) {
    uint32_t crc = s_crcState;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    s_crcState = crc;
}

static uint32_t crc32_finalize() { return ~s_crcState; }

static uint32_t get_u32(const uint8_t* b, int off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static void put_u32(uint8_t* b, int off, uint32_t v) {
    b[off]     = (uint8_t)(v & 0xFF);
    b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static void publish_status(bool notify) {
    if (s_statusChar == nullptr) return;
    uint8_t buf[6];
    put_u32(buf, 0, s_bytesReceived);
    buf[4] = s_state;
    buf[5] = s_error;
    s_statusChar->setValue(buf, sizeof(buf));
    if (notify) s_statusChar->notify();
}

static void fail(uint8_t err, const char* why) {
    if (s_state == OTA_STATE_RECEIVING || s_state == OTA_STATE_VERIFYING) {
        Update.abort();
    }
    s_state = OTA_STATE_ERROR;
    s_error = err;
    Serial.print("OTA:    FAILED at byte ");
    Serial.print(s_bytesReceived);
    Serial.print(" — ");
    Serial.println(why);
    s_statusDirty = true;
}

// -----------------------------------------------------------------------------
// Control (f008) — mailbox only. Validates what it can cheaply, then returns.
// -----------------------------------------------------------------------------
class OtaControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        // Named local, not `c->getValue().data()` inline. On this NimBLE line
        // getValue() returns a std::string BY VALUE, so the copy owns its
        // bytes for as long as it is named — reading .data() off an unnamed
        // temporary is a dangling pointer the moment the statement ends. The
        // bring-up firmware hit exactly that: early bytes "worked" by luck,
        // later reads came back as zeroes. Same shape as ble_command.cpp.
        std::string v = c->getValue();
        if (v.size() < 1) return;

        const uint8_t* d = (const uint8_t*)v.data();
        const uint8_t op = d[0];

        switch (op) {
            case OTA_OP_BEGIN:
                if (v.size() < 9) return;
                s_pendingSize  = get_u32(d, 1);
                s_pendingCrc32 = get_u32(d, 5);
                s_pendingOp    = OTA_OP_BEGIN;
                break;

            case OTA_OP_COMMIT:
            case OTA_OP_ABORT:
                s_pendingOp = op;
                break;

            default:
                Serial.print("OTA:    unknown control op 0x");
                Serial.println(op, HEX);
                break;
        }
    }
};

// -----------------------------------------------------------------------------
// Data (f009) — inline, because this is the fast path and there is nowhere to
// buffer an image this size. Writes straight through to the inactive slot.
// -----------------------------------------------------------------------------
class OtaDataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        // Chunks arriving outside a transfer are dropped in silence rather than
        // raising an error: after an abort or a failure the phone may still have
        // writes in flight, and turning those into a second error would bury the
        // first one, which is the one that explains what happened.
        if (s_state != OTA_STATE_RECEIVING) return;

        std::string v = c->getValue();             // named — see the note in Control
        const size_t len = v.size();
        if (len == 0) return;

        const uint8_t* d = (const uint8_t*)v.data();

        if (s_bytesReceived + len > s_expectedSize) {
            fail(OTA_ERR_SIZE_MISMATCH, "more data than ota_begin declared");
            return;
        }
        if (Update.write((uint8_t*)d, len) != len) {
            fail(OTA_ERR_WRITE_FAILED, Update.errorString());
            return;
        }

        crc32_feed(d, len);
        s_bytesReceived += len;
        s_statusDirty = true;
    }
};

// -----------------------------------------------------------------------------
// The slow half, run from loop().
// -----------------------------------------------------------------------------
static void do_begin() {
    const uint32_t size  = s_pendingSize;
    const uint32_t crc   = s_pendingCrc32;

    // An OTA reboots the board. Mid-session that abandons a vend already paid
    // for, and takes any coins counted but not yet banked with it.
    if (!app_state_is_idle()) {
        s_state = OTA_STATE_ERROR;
        s_error = OTA_ERR_REFUSED_BUSY;
        Serial.println("OTA:    REFUSED — board is not idle (a session or the config menu is open)");
        s_statusDirty = true;
        return;
    }

    if (size < OTA_MIN_IMAGE_BYTES) {
        s_state = OTA_STATE_ERROR;
        s_error = OTA_ERR_SIZE_MISMATCH;
        Serial.print("OTA:    REFUSED — declared size ");
        Serial.print(size);
        Serial.println(" is too small to be a firmware image");
        s_statusDirty = true;
        return;
    }

    s_expectedSize  = size;
    s_expectedCrc32 = crc;
    s_bytesReceived = 0;
    crc32_reset();

    Serial.print("OTA:    begin — ");
    Serial.print(size);
    Serial.println(" bytes; erasing the inactive slot (takes a moment)");
    Serial.flush();

    // Erases the destination slot. Seconds, not milliseconds — which is the
    // whole reason this is here and not in the GATT callback.
    if (!Update.begin(size)) {
        s_state = OTA_STATE_ERROR;
        s_error = OTA_ERR_BEGIN_FAILED;
        Serial.print("OTA:    begin FAILED — ");
        Serial.println(Update.errorString());
        s_statusDirty = true;
        return;
    }

    s_state = OTA_STATE_RECEIVING;
    s_error = OTA_ERR_NONE;
    Serial.println("OTA:    ready to receive");
    s_statusDirty = true;
}

static void do_commit() {
    if (s_state != OTA_STATE_RECEIVING) {
        s_statusDirty = true;      // tell the app what state it is actually in
        return;
    }
    if (s_bytesReceived != s_expectedSize) {
        fail(OTA_ERR_SIZE_MISMATCH, "fewer bytes arrived than ota_begin declared");
        return;
    }

    const uint32_t actual = crc32_finalize();
    if (actual != s_expectedCrc32) {
        fail(OTA_ERR_CRC_MISMATCH, "image checksum does not match");
        return;
    }

    // Idle is checked AGAIN here, not just at begin. Over BLE this transfer
    // takes minutes, which is long enough for someone to drop a coin and start
    // a session while it runs — and commit reboots the board. Rebooting there
    // abandons a vend already paid for.
    //
    // Deliberately REFUSED, not aborted: the image is complete and verified in
    // the inactive slot, and Update's handle is still open, so the phone can
    // simply send ota_commit again once the session ends. Throwing the transfer
    // away would mean re-sending the whole image over a link that needed
    // minutes to carry it the first time.
    if (!app_state_is_idle()) {
        s_error = OTA_ERR_REFUSED_BUSY;
        Serial.println("OTA:    commit deferred — a session is running; send ota_commit again when idle");
        s_statusDirty = true;
        return;
    }

    s_state = OTA_STATE_VERIFYING;
    publish_status(true);

    if (!Update.end(true)) {
        s_state = OTA_STATE_ERROR;
        s_error = OTA_ERR_END_FAILED;
        Serial.print("OTA:    finalise FAILED — ");
        Serial.println(Update.errorString());
        s_statusDirty = true;
        return;
    }

    s_state = OTA_STATE_DONE;
    s_error = OTA_ERR_NONE;
    publish_status(true);

    Serial.println("OTA:    image accepted — rebooting into it");
    Serial.println("        it boots ON TRIAL and reverts unless it proves healthy (AT+OTA?)");
    Serial.flush();

    // Let the notify above actually reach the phone before the radio goes.
    delay(500);

    // Strapping pins and the relay back to a safe state first — on a warm
    // restart that is firmware's job, see src/periph.h.
    periph_reset_safe();
    delay(50);
    esp_restart();
}

static void do_abort() {
    if (s_state == OTA_STATE_RECEIVING || s_state == OTA_STATE_VERIFYING) {
        Update.abort();
    }
    s_state         = OTA_STATE_IDLE;
    s_error         = OTA_ERR_NONE;
    s_bytesReceived = 0;
    Serial.println("OTA:    aborted — the running firmware is untouched");
    s_statusDirty = true;
}

// -----------------------------------------------------------------------------
void ble_ota_register(NimBLEService* service) {
    NimBLECharacteristic* control = service->createCharacteristic(
        BLE_OTA_CONTROL_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
    control->setCallbacks(new OtaControlCallbacks());

    // WRITE_NR as well as WRITE: the app sends chunks without waiting for an
    // ATT response, which is where most of this link's throughput comes from.
    NimBLECharacteristic* data = service->createCharacteristic(
        BLE_OTA_DATA_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    data->setCallbacks(new OtaDataCallbacks());

    s_statusChar = service->createCharacteristic(
        BLE_OTA_STATUS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    publish_status(false);     // a real value before anyone reads it

    Serial.println("BLE: OTA (f008/f009/f00a) ENABLED");
}

void ble_ota_service() {
    const uint8_t op = s_pendingOp;
    if (op != 0) {
        s_pendingOp = 0;
        switch (op) {
            case OTA_OP_BEGIN:  do_begin();  break;
            case OTA_OP_COMMIT: do_commit(); break;
            case OTA_OP_ABORT:  do_abort();  break;
            default: break;
        }
    }

    if (!s_statusDirty) return;

    // While receiving, report at a fixed cadence; every other transition is
    // worth sending the moment it happens.
    const uint32_t now = millis();
    if (s_state == OTA_STATE_RECEIVING) {
        if ((uint32_t)(now - s_lastProgressMs) < OTA_PROGRESS_NOTIFY_MS) return;
        s_lastProgressMs = now;
    }

    s_statusDirty = false;
    publish_status(true);
}

bool ble_ota_in_progress() {
    return s_state == OTA_STATE_RECEIVING || s_state == OTA_STATE_VERIFYING;
}

void ble_ota_print_status(Stream& out) {
    static const char* const names[] = { "idle", "receiving", "verifying", "done", "error" };
    out.print("ota_transfer = ");
    out.println(s_state <= OTA_STATE_ERROR ? names[s_state] : "?");
    if (s_state != OTA_STATE_IDLE) {
        out.print("  bytes = ");
        out.print(s_bytesReceived);
        out.print(" / ");
        out.println(s_expectedSize);
        out.print("  error = ");
        out.println(s_error);
    }
}
