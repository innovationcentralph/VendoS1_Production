#include "ble_wifi.h"

#include "wifi_sta.h"
#include "display.h"

#include <Arduino.h>
#include <string.h>

#define BLE_WIFI_CONFIG_CHAR_UUID "6a40f00b-0000-1000-8000-00805f9b0001"
#define BLE_WIFI_STATUS_CHAR_UUID "6a40f00c-0000-1000-8000-00805f9b0001"

// Generous enough for the spec's limits (32-byte SSID, 63-byte WPA2
// passphrase, 64-byte key) plus their length bytes, and small enough that a
// malformed frame cannot make us read past it.
#define WIFI_SSID_MAX   32
#define WIFI_PASS_MAX   64
#define WIFI_DEVKEY_MAX 64

static NimBLECharacteristic* s_statusChar = nullptr;

// Mailbox — see the header for why the write handler does not act.
static volatile bool s_pending = false;
static char s_p_ssid[WIFI_SSID_MAX + 1];
static char s_p_pass[WIFI_PASS_MAX + 1];
static char s_p_key[WIFI_DEVKEY_MAX + 1];

// Last state pushed to the app, so a notify goes out on change rather than on
// every loop tick.
static uint8_t  s_last_state = 0xFF;
static uint32_t s_last_ip    = 0;

static void publish_status(bool notify) {
    if (s_statusChar == nullptr) return;
    const uint8_t  state = (uint8_t)wifi_state();
    const uint32_t ip    = wifi_ip();

    uint8_t buf[5];
    buf[0] = state;
    // Little-endian, like every other multi-byte field in this contract.
    buf[1] = (uint8_t)(ip & 0xFF);
    buf[2] = (uint8_t)((ip >> 8) & 0xFF);
    buf[3] = (uint8_t)((ip >> 16) & 0xFF);
    buf[4] = (uint8_t)((ip >> 24) & 0xFF);

    s_statusChar->setValue(buf, sizeof(buf));
    if (notify) s_statusChar->notify();
}

// Reads one length-prefixed field, advancing `off`. Returns false if the frame
// is too short for what it claims or the value is longer than we accept —
// either way the whole frame is rejected rather than truncated, because a
// half-read SSID would silently put the board on the wrong network.
static bool take_field(const uint8_t* d, size_t len, size_t& off,
                       char* out, size_t out_cap, size_t max_len) {
    if (off >= len) return false;
    const size_t n = d[off++];
    if (n > max_len)       return false;
    if (off + n > len)     return false;
    if (n >= out_cap)      return false;
    memcpy(out, d + off, n);
    out[n] = '\0';
    off += n;
    return true;
}

class WifiConfigCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();          // named: see ble_ota.cpp
        const uint8_t* d = (const uint8_t*)v.data();
        const size_t len = v.size();

        char ssid[WIFI_SSID_MAX + 1]   = {0};
        char pass[WIFI_PASS_MAX + 1]   = {0};
        char key [WIFI_DEVKEY_MAX + 1] = {0};

        size_t off = 0;
        if (!take_field(d, len, off, ssid, sizeof(ssid), WIFI_SSID_MAX) ||
            !take_field(d, len, off, pass, sizeof(pass), WIFI_PASS_MAX) ||
            !take_field(d, len, off, key,  sizeof(key),  WIFI_DEVKEY_MAX)) {
            Serial.println("BLE WiFi config REFUSED - malformed frame");
            return;
        }
        if (ssid[0] == '\0') {
            Serial.println("BLE WiFi config REFUSED - empty SSID");
            return;
        }

        strncpy(s_p_ssid, ssid, sizeof(s_p_ssid));
        strncpy(s_p_pass, pass, sizeof(s_p_pass));
        strncpy(s_p_key,  key,  sizeof(s_p_key));
        s_pending = true;
    }
};

void ble_wifi_register(NimBLEService* service) {
    NimBLECharacteristic* cfg = service->createCharacteristic(
        BLE_WIFI_CONFIG_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
    cfg->setCallbacks(new WifiConfigCallbacks());

    s_statusChar = service->createCharacteristic(
        BLE_WIFI_STATUS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    publish_status(false);      // a real value before the first read

    Serial.println("BLE: WiFi (f00b/f00c) ENABLED");
}

void ble_wifi_service() {
    if (s_pending) {
        s_pending = false;
        // The SSID is worth printing; the password and key are not. AT+ and
        // boot output get pasted into chats.
        Serial.print("BLE WiFi config - joining \"");
        Serial.print(s_p_ssid);
        Serial.println("\"");
        wifi_provision(s_p_ssid, s_p_pass, s_p_key);
        // Clear our copies once handed over. wifi.cpp owns them from here.
        memset(s_p_pass, 0, sizeof(s_p_pass));
        memset(s_p_key,  0, sizeof(s_p_key));
        publish_status(true);
    }

    const uint8_t  state = (uint8_t)wifi_state();
    const uint32_t ip    = wifi_ip();
    if (state != s_last_state || ip != s_last_ip) {
        s_last_state = state;
        s_last_ip    = ip;
        publish_status(true);

        // Worth saying on the LCD too: "did it work?" is the whole question
        // being asked while someone stands there typing a password in.
        if (state == WIFI_STATE_CONNECTED)   display_show_notice(NOTICE_WIFI_CONNECTED);
        else if (state == WIFI_STATE_FAILED) display_show_notice(NOTICE_WIFI_FAILED);
    }
}
