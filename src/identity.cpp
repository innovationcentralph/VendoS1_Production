#include "identity.h"
#include <Preferences.h>
#include <esp_system.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// One Preferences handle opened and closed per call rather than held open,
// matching config.cpp's convention — these are rare, human-paced operations
// (a factory step and a CLI query), so there is nothing to gain from holding
// the namespace open and something to lose if a reset lands mid-write.

static const size_t kPrefixLen = sizeof(IDENTITY_SERIAL_PREFIX) - 1;

// The serial format is spread across four macros in identity.h, and a serial is
// write-once — a board provisioned against an inconsistent set of them could
// not be corrected without the erase token. So make disagreement a build error
// rather than a field problem. (Same spirit as REVIEW_FINDINGS.md R19's ask for
// a sizeof() assert on AppConfig.)
static_assert(kPrefixLen + IDENTITY_SERIAL_DIGITS == IDENTITY_SERIAL_LEN,
              "IDENTITY_SERIAL_LEN must equal strlen(PREFIX) + DIGITS");
static_assert(IDENTITY_SERIAL_BUF == IDENTITY_SERIAL_LEN + 1,
              "IDENTITY_SERIAL_BUF must leave room for the NUL");
static_assert(sizeof(IDENTITY_SERIAL_PATTERN) == IDENTITY_SERIAL_BUF,
              "IDENTITY_SERIAL_PATTERN's N count must match IDENTITY_SERIAL_DIGITS");
static_assert(sizeof(IDENTITY_SERIAL_EXAMPLE) == IDENTITY_SERIAL_BUF,
              "IDENTITY_SERIAL_EXAMPLE's digit count must match IDENTITY_SERIAL_DIGITS");

bool identity_serial_format_ok(const char* s) {
    if (s == nullptr) return false;
    if (strlen(s) != IDENTITY_SERIAL_LEN) return false;

    for (size_t i = 0; i < kPrefixLen; ++i) {
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)IDENTITY_SERIAL_PREFIX[i])) {
            return false;
        }
    }
    for (size_t i = kPrefixLen; i < IDENTITY_SERIAL_LEN; ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// Reads the raw stored string. Returns false if absent or unreadable; does not
// judge the format (callers that care use identity_serial_valid()).
static bool stored_serial_read(char out[IDENTITY_SERIAL_BUF]) {
    out[0] = '\0';

    Preferences prefs;
    if (!prefs.begin(IDENTITY_NS, /*readOnly=*/true)) {
        // A namespace that has never been written does not exist yet, so this
        // is the normal path on an unprovisioned board — not an error.
        return false;
    }
    const size_t n = prefs.getString(IDENTITY_KEY, out, IDENTITY_SERIAL_BUF);
    prefs.end();

    if (n == 0) { out[0] = '\0'; return false; }
    out[IDENTITY_SERIAL_BUF - 1] = '\0';
    return true;
}

bool identity_serial_valid() {
    char s[IDENTITY_SERIAL_BUF];
    if (!stored_serial_read(s)) return false;
    return identity_serial_format_ok(s);
}

bool identity_serial_get(char out[IDENTITY_SERIAL_BUF]) {
    if (!stored_serial_read(out) || !identity_serial_format_ok(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

IdentitySetResult identity_serial_set(const char* s) {
    if (!identity_serial_format_ok(s)) return IDENTITY_SET_BAD_FORMAT;

    // Write-once. Checked here rather than in the CLI so every future caller
    // (a BLE provisioning path, a factory jig) inherits the same rule instead
    // of having to remember it.
    if (identity_serial_valid()) return IDENTITY_SET_ALREADY;

    // Store the canonical spelling of the prefix regardless of how it was
    // typed, so two boards cannot differ only by case.
    char canonical[IDENTITY_SERIAL_BUF];
    memcpy(canonical, IDENTITY_SERIAL_PREFIX, kPrefixLen);
    memcpy(canonical + kPrefixLen, s + kPrefixLen, IDENTITY_SERIAL_DIGITS);
    canonical[IDENTITY_SERIAL_LEN] = '\0';

    Preferences prefs;
    if (!prefs.begin(IDENTITY_NS, /*readOnly=*/false)) return IDENTITY_SET_NVS_FAIL;
    const size_t n = prefs.putString(IDENTITY_KEY, canonical);
    prefs.end();

    if (n == 0) return IDENTITY_SET_NVS_FAIL;

    // Read back rather than trusting the write. This is the one value on the
    // board that cannot be corrected in the field without the erase token, so
    // it is worth the extra NVS read to fail loudly at the factory instead.
    return identity_serial_valid() ? IDENTITY_SET_OK : IDENTITY_SET_NVS_FAIL;
}

bool identity_serial_erase(const char* token) {
    char expect[IDENTITY_TOKEN_BUF];
    identity_erase_token(expect);

    if (token == nullptr) return false;
    if (strlen(token) != IDENTITY_TOKEN_LEN) return false;
    for (size_t i = 0; i < IDENTITY_TOKEN_LEN; ++i) {
        if (toupper((unsigned char)token[i]) != expect[i]) return false;
    }

    Preferences prefs;
    if (!prefs.begin(IDENTITY_NS, /*readOnly=*/false)) return false;
    const bool ok = prefs.remove(IDENTITY_KEY);
    prefs.end();
    return ok;
}

void identity_mac(uint8_t out[6]) {
    // esp_efuse_mac_get_default gives the base MAC in canonical byte order,
    // unlike ESP.getEfuseMac()'s reversed 64-bit packing. On any failure the
    // buffer is zeroed, so a caller sees 00:00:00:00:00:00 rather than stack
    // garbage that would look like a plausible address.
    if (esp_efuse_mac_get_default(out) != ESP_OK) {
        memset(out, 0, 6);
    }
}

void identity_erase_token(char out[IDENTITY_TOKEN_BUF]) {
    uint8_t mac[6];
    identity_mac(mac);
    snprintf(out, IDENTITY_TOKEN_BUF, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void identity_print(Stream& out) {
    char serial[IDENTITY_SERIAL_BUF];
    uint8_t mac[6];
    identity_mac(mac);

    if (identity_serial_get(serial)) {
        out.print("Serial: "); out.println(serial);
    } else {
        out.println("Serial: UNPROVISIONED  (set with AT+SERIAL=" IDENTITY_SERIAL_PATTERN ")");
    }

    char macstr[18];
    snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    out.print("MAC:    "); out.println(macstr);
}
