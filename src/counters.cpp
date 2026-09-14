#include "counters.h"
#include "rtc.h"
#include <Preferences.h>
#include "diag.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

// Persisted blob. Versioned for the same reason AppConfig has a magic: a layout
// change must present as "start again", not as misparsed takings.
#define COUNTERS_BLOB_VERSION 1

struct CountersBlob {
    uint8_t  version;
    uint8_t  _pad[3];
    uint32_t today_day;         // business-day key these today_* belong to
    uint32_t today_amount;
    uint16_t today_sessions;
    uint16_t _pad2;
    uint32_t lifetime_amount;
    uint32_t last_seq;
};

static CountersBlob      s_c        = {};
static SemaphoreHandle_t s_lock     = nullptr;
static bool              s_dirty    = false;   // for BLE notify
static bool              s_unsaved  = false;   // for the NVS flush
static uint32_t          s_last_change_ms = 0;

// Day key meaning "the clock was not trustworthy when this was recorded".
// Distinct from any real day, so the first valid timestamp rolls it over and
// discards takings that could not be attributed to a real business day. Lifetime
// is unaffected — that one is always meaningful.
#define COUNTERS_DAY_UNKNOWN 0u

struct Lock {
    bool held;
    Lock() : held(false) {
        if (s_lock) held = (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE);
    }
    ~Lock() { if (held) xSemaphoreGive(s_lock); }
};

// ============================================================================
// Business day
// ============================================================================

uint32_t counters_business_day(uint32_t epoch_utc) {
    if (epoch_utc == 0) return COUNTERS_DAY_UNKNOWN;
    // Shift into Manila local time, then take whole days. Manila midnight is
    // 16:00 UTC the previous day; adding the offset before dividing is what
    // moves the boundary there. +1 so a real day is never 0 and therefore never
    // collides with COUNTERS_DAY_UNKNOWN.
    return ((epoch_utc + (uint32_t)COUNTERS_TZ_OFFSET_MIN * 60UL) / 86400UL) + 1u;
}

// ============================================================================
// NVS
// ============================================================================

static void counters_load_locked() {
    memset(&s_c, 0, sizeof(s_c));
    s_c.version = COUNTERS_BLOB_VERSION;

    Preferences prefs;
    if (!prefs.begin(COUNTERS_NS, /*readOnly=*/true)) return;   // never written yet
    CountersBlob stored;
    const size_t n = prefs.getBytes(COUNTERS_KEY, &stored, sizeof(stored));
    prefs.end();

    // A short blob or a stale version both mean "no usable history". Starting
    // from zero is the honest outcome; silently reinterpreting old bytes as
    // takings is not.
    if (n != sizeof(stored) || stored.version != COUNTERS_BLOB_VERSION) return;
    s_c = stored;
}

static void counters_save_locked() {
    Preferences prefs;
    if (!prefs.begin(COUNTERS_NS, /*readOnly=*/false)) {
        diag_raise(DIAG_ERR_NVS_WRITE);
        return;                       // stay dirty; retry on the next service tick
    }
    const size_t n = prefs.putBytes(COUNTERS_KEY, &s_c, sizeof(s_c));
    prefs.end();

    // The result was previously discarded. A silently failing flush is the
    // nastiest of the NVS failures: the board keeps counting correctly in RAM
    // and loses it at every reboot, so lifetime takings appear to go BACKWARDS
    // — and that figure is the operator's meter reading against cash in the box.
    if (n != sizeof(s_c)) {
        diag_raise(DIAG_ERR_NVS_WRITE);
        return;                       // leave s_unsaved set so we try again
    }
    s_unsaved = false;
}

// Rolls today_* over if the business day has moved on. Caller holds the lock.
static void counters_roll_day_locked() {
    const uint32_t day = counters_business_day(rtc_now());
    if (day == s_c.today_day) return;

    // A day boundary is worth persisting immediately: it is the one moment the
    // previous day's total becomes final, and losing it to a reset would put a
    // permanent hole in the operator's history.
    s_c.today_day      = day;
    s_c.today_amount   = 0;
    s_c.today_sessions = 0;
    s_dirty            = true;
    counters_save_locked();
}

// ============================================================================
// Recording
// ============================================================================

void counters_record_coin(uint32_t cents) {
    Lock lock;
    if (!lock.held) return;
    counters_roll_day_locked();

    // Saturating rather than wrapping. u32 centavos is ~P42.9 million, which a
    // single machine will not reach — but if it somehow does, a total that
    // sticks at the ceiling is a visible anomaly, where one that wraps to near
    // zero reads as a plausible number and quietly corrupts the history.
    if (s_c.lifetime_amount > UINT32_MAX - cents) s_c.lifetime_amount = UINT32_MAX;
    else                                          s_c.lifetime_amount += cents;

    if (s_c.today_amount > UINT32_MAX - cents) s_c.today_amount = UINT32_MAX;
    else                                       s_c.today_amount += cents;

    s_dirty          = true;
    s_unsaved        = true;
    s_last_change_ms = millis();
}

void counters_record_session() {
    Lock lock;
    if (!lock.held) return;
    counters_roll_day_locked();
    // One paid period, not one relay edge and not one pulse — see counters.h.
    if (s_c.today_sessions < UINT16_MAX) ++s_c.today_sessions;
    s_dirty          = true;
    s_unsaved        = true;
    s_last_change_ms = millis();
}

void counters_set_last_seq(uint32_t seq) {
    Lock lock;
    if (!lock.held) return;
    s_c.last_seq     = seq;
    s_dirty          = true;
    s_unsaved        = true;
    s_last_change_ms = millis();
}

// ============================================================================
// Reporting
// ============================================================================

static void put_u16(uint8_t* b, size_t off, uint16_t v) {
    b[off] = (uint8_t)(v & 0xFF); b[off + 1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t* b, size_t off, uint32_t v) {
    b[off]     = (uint8_t)(v & 0xFF);
    b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

void counters_serialize(uint8_t out[COUNTERS_WIRE_LEN]) {
    Lock lock;
    // Offsets are fixed by the app's decodeLiveCounters(); note today_sessions
    // is a u16 at 4, which leaves lifetime_amount on an ODD offset (6). That is
    // the contract, not an oversight — do not pad it.
    //
    // /100 is THE centavos->pesos boundary (see counters.h). Exact, because
    // price-per-pulse is always whole pesos.
    put_u32(out, 0,  s_c.today_amount / 100u);
    put_u16(out, 4,  s_c.today_sessions);
    put_u32(out, 6,  s_c.lifetime_amount / 100u);
    put_u32(out, 10, s_c.last_seq);
}

bool counters_take_dirty() {
    Lock lock;
    if (!lock.held) return false;
    const bool d = s_dirty;
    s_dirty = false;
    return d;
}

uint32_t counters_today_amount()    { Lock l; return s_c.today_amount; }
uint16_t counters_today_sessions()  { Lock l; return s_c.today_sessions; }
uint32_t counters_lifetime_amount() { Lock l; return s_c.lifetime_amount; }
uint32_t counters_last_seq()        { Lock l; return s_c.last_seq; }

// ============================================================================
// Service + init
// ============================================================================

void counters_service() {
    Lock lock;
    if (!lock.held) return;

    counters_roll_day_locked();

    if (s_unsaved && (uint32_t)(millis() - s_last_change_ms) >= COUNTERS_FLUSH_IDLE_MS) {
        counters_save_locked();
    }
}

void counters_init() {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        Serial.println("WARN: counters mutex creation failed — earnings will not be recorded");
        return;
    }
    Lock lock;
    if (!lock.held) return;
    counters_load_locked();
    counters_roll_day_locked();
}

void counters_print(Stream& out) {
    const uint32_t now = rtc_now();
    out.print("today    = P");
    out.print(counters_today_amount() / 100); out.print('.');
    const uint32_t frac = counters_today_amount() % 100;
    if (frac < 10) out.print('0');
    out.println(frac);

    out.print("sessions = "); out.print(counters_today_sessions());
    out.println("   (completed vends today, not pulses)");

    out.print("lifetime = P");
    out.print(counters_lifetime_amount() / 100); out.print('.');
    const uint32_t lfrac = counters_lifetime_amount() % 100;
    if (lfrac < 10) out.print('0');
    out.println(lfrac);

    out.print("last_seq = "); out.println(counters_last_seq());
    out.print("on the wire: today="); out.print(counters_today_amount() / 100u);
    out.print(" lifetime="); out.print(counters_lifetime_amount() / 100u);
    out.println("   (PESOS — the app does not divide by 100)");

    out.print("day key  = ");
    if (s_c.today_day == COUNTERS_DAY_UNKNOWN) {
        out.println("UNKNOWN — clock was not set, today's figures are not attributable");
        out.println("  set the clock (AT+RTC=) and today_* will reset to a real business day");
    } else {
        out.print(s_c.today_day);
        out.println("  (Asia/Manila business day, rolls at 16:00 UTC)");
    }

    out.print("clock    = ");
    out.println(now == 0 ? "NOT SET" : "set");
    out.print("unsaved  = ");
    out.println(s_unsaved ? "yes (flush pending)" : "no");
}
