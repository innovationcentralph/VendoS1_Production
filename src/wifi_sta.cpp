#include "wifi_sta.h"

#include "diag.h"
#include "display.h"

#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

// Its own namespace, beside `vendoid` and `vendocnt` rather than inside the
// config blob. Same standing rule as those: a magic bump or a factory reset
// must not take a board off the network, and config_load() falls back to
// defaults on a magic mismatch — which would silently erase credentials as a
// side effect of that guard working correctly. See CLAUDE.md, Storage.
#define WIFI_NS          "vendowifi"
#define WIFI_KEY_SSID    "ssid"
#define WIFI_KEY_PASS    "pass"
#define WIFI_KEY_DEVKEY  "devkey"

#define WIFI_SSID_BUF    33    // 32 + NUL
#define WIFI_PASS_BUF    65    // 64 + NUL

// How long one association attempt is given before it counts as failed.
#define WIFI_ATTEMPT_MS      20000

// How long to wait before trying again after a failure. Deliberately long: a
// board with a wrong password or a router that is off retries forever, and
// each attempt costs radio time the BLE link is sharing. A minute is far
// faster than any human would notice and cheap enough to run all day.
#define WIFI_RETRY_MS        60000

static WifiConnState s_state       = WIFI_STATE_IDLE;
static char          s_ssid[WIFI_SSID_BUF]   = {0};
static char          s_pass[WIFI_PASS_BUF]   = {0};
static char          s_devkey[WIFI_DEVICE_KEY_BUF] = {0};
static uint32_t      s_attempt_started_ms    = 0;
static uint32_t      s_last_failure_ms       = 0;
static bool          s_begun                 = false;   // WiFi.begin() called this attempt

static void load_credentials() {
    Preferences prefs;
    if (!prefs.begin(WIFI_NS, /*readOnly=*/true)) return;   // namespace absent = never provisioned
    prefs.getString(WIFI_KEY_SSID,   s_ssid,   sizeof(s_ssid));
    prefs.getString(WIFI_KEY_PASS,   s_pass,   sizeof(s_pass));
    prefs.getString(WIFI_KEY_DEVKEY, s_devkey, sizeof(s_devkey));
    prefs.end();
}

static void start_attempt() {
    s_state              = WIFI_STATE_CONNECTING;
    s_attempt_started_ms = millis();
    s_begun              = true;

    // Station mode only. The board is a client; it never offers an access
    // point, which would be a second unauthenticated way in.
    WiFi.mode(WIFI_STA);

    // Returns immediately — association happens on the WiFi stack's own task
    // (core 0, see CLAUDE.md's task layout). Polled in wifi_service().
    WiFi.begin(s_ssid, s_pass);

    // Said on the LCD, not just the serial port. An attempt takes up to 20 s,
    // and a blank screen for that long is how an operator decides the board is
    // dead and pulls the plug partway through.
    display_show_notice(NOTICE_WIFI_CONNECTING);

    Serial.print("WiFi:   connecting to \"");
    Serial.print(s_ssid);
    Serial.println("\"");
}

void wifi_init() {
    load_credentials();

    if (s_ssid[0] == '\0') {
        s_state = WIFI_STATE_IDLE;
        Serial.println("WiFi:   not provisioned — BLE is this board's only link");
        return;
    }

    // Stops the stack writing its own copy of the credentials to NVS on every
    // connect. We persist them ourselves, in our own namespace, and two
    // sources of truth is how a board ends up reconnecting to a network it was
    // explicitly told to forget.
    WiFi.persistent(false);

    // DO NOT call WiFi.setSleep(false) here.
    //
    // It looks right on a mains-powered board — keep the radio awake, answer
    // promptly — and it bricks the boot when BLE is also running:
    //
    //   E (1937) wifi: Error! Should enable WiFi modem sleep when both WiFi
    //   and Bluetooth are enabled!!!!!!
    //
    // ...followed by a reboot loop. WiFi and BLE share one radio on this chip,
    // and the coexistence arbiter needs the WiFi side to yield time slots;
    // modem sleep is how it does that. Leaving the default on is not a power
    // optimisation here, it is what makes the two coexist at all.
    //
    // Found on hardware 2026-10-16, on the first provisioning attempt.

    start_attempt();
}

void wifi_service() {
    if (s_ssid[0] == '\0') return;     // never provisioned; nothing to drive

    const uint32_t now = millis();

    if (s_state == WIFI_STATE_CONNECTING) {
        // Keep the message on screen for the whole attempt.
        //
        // A notice holds for 2.5 s and an attempt takes 20, so the LCD showed
        // "connecting" for a moment and then went back to idle for seventeen
        // seconds before saying it failed — which reads as nothing happening
        // at all, which is exactly what it was added to prevent. Re-raising it
        // keeps it up without needing a second kind of sticky screen, and it
        // still yields to a vend: the display task drops notices whenever a
        // session or config page is showing.
        static uint32_t last_nudge_ms = 0;
        if ((uint32_t)(now - last_nudge_ms) >= 2000) {
            last_nudge_ms = now;
            display_show_notice(NOTICE_WIFI_CONNECTING);
        }

        if (WiFi.status() == WL_CONNECTED) {
            s_state = WIFI_STATE_CONNECTED;
            display_show_notice(NOTICE_WIFI_CONNECTED);
            Serial.print("WiFi:   connected — ");
            Serial.println(WiFi.localIP().toString());
            return;
        }
        if ((uint32_t)(now - s_attempt_started_ms) >= WIFI_ATTEMPT_MS) {
            s_state           = WIFI_STATE_FAILED;
            s_last_failure_ms = now;
            s_begun           = false;

            // Disconnect WITHOUT powering the radio down. wifioff=true made
            // the stack log "timeout when WiFi un-init, type=4" on every
            // failed attempt — it tears the driver down and brings it back a
            // minute later for the retry, which is both slower and noisier
            // than simply dropping the association.
            WiFi.disconnect(/*wifioff=*/false);

            display_show_notice(NOTICE_WIFI_FAILED);
            Serial.println("WiFi:   could not connect to that network");
            // 2.4 GHz first, because it is the one that cannot be fixed by
            // retyping anything: this chip has no 5 GHz radio, and a router
            // advertising one SSID across both bands looks to an operator like
            // a network the board is simply refusing to join.
            Serial.println("        - this board is 2.4 GHz ONLY; it cannot see a 5 GHz network");
            Serial.println("        - check the password");
            Serial.println("        - check the network reaches the machine");
            Serial.println("        retrying in a minute; BLE is unaffected");
        }
        return;
    }

    if (s_state == WIFI_STATE_CONNECTED) {
        // Dropped out from under us: an AP reboot, the machine moved, or
        // interference. Back to connecting rather than failed — nothing is
        // known to be wrong with the credentials.
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi:   link dropped — reconnecting");
            start_attempt();
        }
        return;
    }

    if (s_state == WIFI_STATE_FAILED) {
        if ((uint32_t)(now - s_last_failure_ms) >= WIFI_RETRY_MS) start_attempt();
        return;
    }

    // IDLE with credentials present happens after wifi_init() stored some but
    // something stopped the first attempt. Pick it up.
    if (s_state == WIFI_STATE_IDLE && !s_begun) start_attempt();
}

bool wifi_provision(const char* ssid, const char* password, const char* device_key) {
    if (ssid == nullptr || ssid[0] == '\0') return false;

    Preferences prefs;
    if (!prefs.begin(WIFI_NS, /*readOnly=*/false)) {
        diag_raise(DIAG_ERR_NVS_WRITE);
        Serial.println("WiFi:   could not open storage — credentials NOT saved");
        return false;
    }
    // Persisted BEFORE connecting, deliberately. A board that reboots during
    // the attempt should come back trying, not come back having forgotten what
    // it was told — the operator who typed the password may be long gone.
    prefs.putString(WIFI_KEY_SSID,   ssid);
    prefs.putString(WIFI_KEY_PASS,   password ? password : "");
    prefs.putString(WIFI_KEY_DEVKEY, device_key ? device_key : "");
    prefs.end();

    strncpy(s_ssid,   ssid,                        sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass,   password   ? password   : "", sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
    strncpy(s_devkey, device_key ? device_key : "", sizeof(s_devkey) - 1);
    s_devkey[sizeof(s_devkey) - 1] = '\0';

    WiFi.persistent(false);   // see wifi_init() — and do NOT add setSleep(false)
    WiFi.disconnect(/*wifioff=*/false);   // drop any previous association first
    start_attempt();

    Serial.print("WiFi:   provisioned");
    Serial.println(s_devkey[0] ? " with a backend key — check-ins enabled"
                               : " WITHOUT a backend key — connects, but will not check in");
    return true;
}

void wifi_forget() {
    Preferences prefs;
    if (prefs.begin(WIFI_NS, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }
    memset(s_ssid,   0, sizeof(s_ssid));
    memset(s_pass,   0, sizeof(s_pass));
    memset(s_devkey, 0, sizeof(s_devkey));

    WiFi.disconnect(/*wifioff=*/true);
    WiFi.mode(WIFI_OFF);
    s_state = WIFI_STATE_IDLE;
    s_begun = false;

    Serial.println("WiFi:   forgotten — credentials and backend key erased, radio off");
}

WifiConnState wifi_state() { return s_state; }

uint32_t wifi_ip() {
    if (s_state != WIFI_STATE_CONNECTED) return 0;
    return (uint32_t)WiFi.localIP();
}

bool wifi_provisioned()     { return s_ssid[0]   != '\0'; }
bool wifi_has_device_key()  { return s_devkey[0] != '\0'; }

bool wifi_device_key(char out[WIFI_DEVICE_KEY_BUF]) {
    if (s_devkey[0] == '\0') return false;
    strncpy(out, s_devkey, WIFI_DEVICE_KEY_BUF - 1);
    out[WIFI_DEVICE_KEY_BUF - 1] = '\0';
    return true;
}

void wifi_print_status(Stream& out) {
    static const char* const names[] = { "idle", "connecting", "connected", "failed" };
    out.print("wifi_state = ");
    out.println(s_state <= WIFI_STATE_FAILED ? names[s_state] : "?");

    out.print("provisioned = ");
    out.println(s_ssid[0] ? "yes" : "no");
    if (s_ssid[0]) {
        out.print("ssid = ");
        out.println(s_ssid);
    }

    // The key itself is never printed. It authorises this machine's check-ins,
    // and AT+ output routinely ends up pasted into chats and issue threads.
    out.print("device_key = ");
    out.println(s_devkey[0] ? "set (not shown)" : "not set — check-ins disabled");

    out.print("ip = ");
    out.println(s_state == WIFI_STATE_CONNECTED ? WiFi.localIP().toString() : String("—"));

    if (s_state == WIFI_STATE_CONNECTED) {
        out.print("rssi = ");
        out.println(WiFi.RSSI());
    }
    out.println("OK");
}
