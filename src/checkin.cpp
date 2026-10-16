#include "checkin.h"

#include "backend_ca.h"
#include "eventlog.h"
#include "identity.h"
#include "version.h"
#include "wifi_sta.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

// Where the backend lives. A build-time constant rather than something sent at
// provisioning time: the contract's WiFi Config frame carries an SSID, a
// password and a device key, and nothing else. A board that could be pointed
// at an arbitrary host by anyone within BLE range would also be a board that
// could be made to post its earnings somewhere else.
#ifndef CHECKIN_BASE_URL
#define CHECKIN_BASE_URL "https://app-api.vendolabsph.com"
#endif

#define CHECKIN_INTERVAL_MS   60000    // a minute between successful check-ins
#define CHECKIN_IDLE_POLL_MS   5000    // re-check preconditions this often
#define CHECKIN_HTTP_TIMEOUT  15000

#define CHECKIN_TASK_STACK    10240    // TLS handshake needs far more than the app tasks
#define CHECKIN_TASK_PRIO         1    // below display (2): never delays a coin or a relay
#define CHECKIN_TASK_CORE         1

// Highest seq the backend has accepted. RAM only, like the BLE sync_ack: a
// reboot simply re-sends, and the server de-duplicates on (machine, seq). The
// alternative is writing to NVS on every check-in, which is a flash write a
// minute, forever, to save re-posting a handful of rows after a power cut.
static uint32_t s_acked_seq      = 0;
static uint32_t s_last_ok_ms     = 0;
static bool     s_ever_ok        = false;
static int      s_last_http_code = 0;
static uint32_t s_sent_total     = 0;

// ISO 8601 UTC, which is what the backend parses. Returns false when the clock
// has never been set — the row is still sent, flagged tsUnverified, exactly as
// the BLE path does. Filing a vend under 1970 would be worse than admitting
// the timestamp is unknown.
static bool iso8601(uint32_t epoch, char* out, size_t cap) {
    if (epoch == 0) return false;
    const time_t t = (time_t)epoch;
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    return strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0;
}

// Appends one JSON session row from the 14-byte wire record. Layout is
// eventlog.h's: seq u32, ts u32, ts_unverified u8, denom u8, amount u32.
static size_t append_session(char* buf, size_t cap, size_t len, const uint8_t* row, bool first) {
    const uint32_t seq    = (uint32_t)row[0] | ((uint32_t)row[1] << 8) | ((uint32_t)row[2] << 16) | ((uint32_t)row[3] << 24);
    const uint32_t ts     = (uint32_t)row[4] | ((uint32_t)row[5] << 8) | ((uint32_t)row[6] << 16) | ((uint32_t)row[7] << 24);
    const uint8_t  unver  = row[8];
    const uint8_t  denom  = row[9];
    const uint32_t amount = (uint32_t)row[10] | ((uint32_t)row[11] << 8) | ((uint32_t)row[12] << 16) | ((uint32_t)row[13] << 24);

    char when[32];
    const bool have_ts = iso8601(ts, when, sizeof(when));
    if (!have_ts) {
        // Something parseable is still required by the wire contract; the flag
        // beside it is what tells the backend not to trust it.
        strncpy(when, "1970-01-01T00:00:00Z", sizeof(when));
        when[sizeof(when) - 1] = '\0';
    }

    return len + snprintf(buf + len, cap - len,
        "%s{\"seq\":%lu,\"occurredAt\":\"%s\",\"denom\":%u,\"amount\":%lu,\"tsUnverified\":%s}",
        first ? "" : ",",
        (unsigned long)seq, when, (unsigned)denom, (unsigned long)amount,
        (unver || !have_ts) ? "true" : "false");
}

// Builds the body and returns the highest seq it contains (0 if none).
static uint32_t build_payload(char* buf, size_t cap, size_t* out_len) {
    char serial[IDENTITY_SERIAL_BUF];
    if (!identity_serial_get(serial)) serial[0] = '\0';

    char now[32];
    const bool have_now = iso8601((uint32_t)time(nullptr), now, sizeof(now));

    size_t len = 0;
    len += snprintf(buf + len, cap - len, "{\"firmwareVersion\":\"%s\"", FW_VERSION_FULL);

    if (have_now) {
        len += snprintf(buf + len, cap - len,
            ",\"heartbeat\":{\"ts\":\"%s\",\"uptime\":%lu}", now, (unsigned long)(millis() / 1000));
    }

    uint8_t page[EVENTLOG_PAGE_MAX_LEN];
    const size_t page_len = eventlog_build_page(s_acked_seq, page);
    const uint8_t count = (page_len >= 2) ? page[0] : 0;

    uint32_t highest = 0;
    if (count > 0) {
        len += snprintf(buf + len, cap - len, ",\"sessions\":[");
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t* row = page + 2 + (size_t)i * EVENTLOG_WIRE_LEN;
            len = append_session(buf, cap, len, row, i == 0);
            const uint32_t seq = (uint32_t)row[0] | ((uint32_t)row[1] << 8) |
                                 ((uint32_t)row[2] << 16) | ((uint32_t)row[3] << 24);
            if (seq > highest) highest = seq;
        }
        len += snprintf(buf + len, cap - len, "]");
    }

    len += snprintf(buf + len, cap - len, "}");
    *out_len = len;
    return highest;
}

static void checkin_once() {
    char serial[IDENTITY_SERIAL_BUF];
    char devkey[WIFI_DEVICE_KEY_BUF];
    if (!identity_serial_get(serial) || !wifi_device_key(devkey)) return;

    // One page of sessions plus the envelope. EVENTLOG_PAGE_MAX_EVENTS is 10
    // and a row is under 100 characters of JSON, so 2 KB is comfortable — and
    // snprintf is bounded regardless, so the worst case is a truncated body
    // the server rejects, not a smashed stack.
    static char body[2048];
    size_t len = 0;
    const uint32_t highest = build_payload(body, sizeof(body), &len);

    WiFiClientSecure client;
    // Authenticates the server. See backend_ca.h for why this is not
    // setInsecure(): the device key travels in a header on this request.
    client.setCACert(BACKEND_ROOT_CA);
    client.setTimeout(CHECKIN_HTTP_TIMEOUT / 1000);

    char url[160];
    snprintf(url, sizeof(url), "%s/api/board/%s/checkin", CHECKIN_BASE_URL, serial);

    HTTPClient http;
    http.setTimeout(CHECKIN_HTTP_TIMEOUT);
    if (!http.begin(client, url)) {
        Serial.println("CHECKIN: could not start the request");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Key", devkey);

    const int code = http.POST((uint8_t*)body, len);
    s_last_http_code = code;

    if (code == 200) {
        // The response carries a pending config and possibly a firmware URL.
        // Both are deliberately ignored for now — see checkin.h.
        if (highest > s_acked_seq) {
            s_sent_total += (highest - s_acked_seq);
            s_acked_seq = highest;
        }
        s_last_ok_ms = millis();
        s_ever_ok    = true;
        Serial.print("CHECKIN: ok");
        if (highest) { Serial.print(" — sessions up to seq "); Serial.print(highest); }
        Serial.println();
    } else if (code == 401) {
        // The key is wrong or the machine was removed. Retrying cannot fix
        // either, but saying so beats a silent stop.
        Serial.println("CHECKIN: REFUSED (401) — this board's key is not accepted;");
        Serial.println("         set WiFi up again from the app to mint a new one");
    } else {
        Serial.print("CHECKIN: failed, HTTP ");
        Serial.println(code);
    }
    http.end();
}

static void checkin_task(void* arg) {
    (void)arg;
    for (;;) {
        // Every precondition is re-checked each pass rather than latched: WiFi
        // comes and goes, and a board can be provisioned or forgotten at any
        // moment while this task is asleep.
        const bool ready = wifi_state() == WIFI_STATE_CONNECTED &&
                           wifi_has_device_key() &&
                           identity_serial_valid();
        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(CHECKIN_IDLE_POLL_MS));
            continue;
        }
        checkin_once();
        vTaskDelay(pdMS_TO_TICKS(CHECKIN_INTERVAL_MS));
    }
}

void checkin_init() {
    if (xTaskCreatePinnedToCore(checkin_task, "checkin", CHECKIN_TASK_STACK, nullptr,
                                CHECKIN_TASK_PRIO, nullptr, CHECKIN_TASK_CORE) != pdPASS) {
        Serial.println("WARN: check-in task creation failed — this board will not report over WiFi");
    }
}

void checkin_print_status(Stream& out) {
    out.print("backend = ");
    out.println(CHECKIN_BASE_URL);

    out.print("eligible = ");
    if (wifi_state() != WIFI_STATE_CONNECTED) out.println("no — WiFi not connected");
    else if (!wifi_has_device_key())          out.println("no — no device key (set WiFi up from the app)");
    else if (!identity_serial_valid())        out.println("no — board has no serial");
    else                                      out.println("yes");

    out.print("last_result = ");
    if (s_last_http_code == 0) out.println("never attempted");
    else { out.print("HTTP "); out.println(s_last_http_code); }

    if (s_ever_ok) {
        out.print("last_ok_s_ago = ");
        out.println((uint32_t)((millis() - s_last_ok_ms) / 1000));
    } else {
        out.println("last_ok_s_ago = never");
    }

    out.print("acked_seq = ");
    out.println(s_acked_seq);
    out.print("sessions_sent = ");
    out.println(s_sent_total);
    out.println("OK");
}
