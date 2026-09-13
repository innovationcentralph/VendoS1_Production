#include "rtc.h"
#include "pins.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <stdio.h>

// --- DS1302 register addresses, already shifted into command-byte form.
// Command byte layout: bit7 = 1 always, bit6 = 0 clock / 1 RAM,
// bits 5..1 = address, bit0 = 1 read / 0 write. So each register is an
// even "write" value; OR in 1 to read it.
#define RTC_SEC     0x80
#define RTC_MIN     0x82
#define RTC_HOUR    0x84
#define RTC_DATE    0x86
#define RTC_MONTH   0x88
#define RTC_DOW     0x8A
#define RTC_YEAR    0x8C
#define RTC_WP      0x8E
#define RTC_TRICKLE 0x90

#define RTC_CH_BIT  0x80    // seconds bit7: 1 = clock halted
#define RTC_WP_BIT  0x80    // WP  bit7: 1 = writes blocked
#define RTC_12H_BIT 0x80    // hour bit7: 1 = 12-hour mode

// 2us per half-cycle is ~250kHz, well inside the DS1302's 1MHz limit at 3.3V
// and slow enough not to care about the wiring. A whole frame is a few tens of
// microseconds, so nothing here needs to yield.
#define RTC_TICK_US 2

// Write the system clock back to the chip this often. An hour is a compromise:
// often enough that a power cut loses little, rare enough that the cell and the
// bus see almost no traffic.
#define RTC_WRITEBACK_INTERVAL_MS (60UL * 60UL * 1000UL)

static SemaphoreHandle_t s_lock       = nullptr;
static bool              s_time_set   = false;  // has the system clock been established this boot?
static uint32_t          s_last_wb_ms = 0;

// Every public entry point takes this. The harness had no need for it — its CLI
// was the only caller — but here the app task, the CLI in loop() and (later) a
// BLE callback can all arrive concurrently. Bit-banging is safe to *preempt*
// (the DS1302 is static, with no minimum clock rate, so a frame stretched by a
// context switch still completes correctly) but it is NOT safe to interleave:
// two frames sharing one data line corrupt both.
struct RtcLock {
    bool held;
    RtcLock() : held(false) {
        if (s_lock) held = (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE);
    }
    ~RtcLock() { if (held) xSemaphoreGive(s_lock); }
};

// ============================================================================
// 3-wire primitives — ported verbatim from the bring-up harness
// ============================================================================

static void rtc_begin_frame() {
    digitalWrite(PIN_RTC_CE, LOW);
    digitalWrite(PIN_RTC_CLK, LOW);
    delayMicroseconds(RTC_TICK_US);
    digitalWrite(PIN_RTC_CE, HIGH);     // CE high for the whole transfer
    delayMicroseconds(RTC_TICK_US);
}

static void rtc_end_frame() {
    digitalWrite(PIN_RTC_CE, LOW);
    digitalWrite(PIN_RTC_CLK, LOW);
    pinMode(PIN_RTC_DAT, INPUT);        // release the shared data line
    delayMicroseconds(RTC_TICK_US);
}

// LSB first, latched by the chip on each rising edge.
static void rtc_write_byte(uint8_t v) {
    pinMode(PIN_RTC_DAT, OUTPUT);
    for (uint8_t i = 0; i < 8; ++i) {
        digitalWrite(PIN_RTC_DAT, (v >> i) & 1);
        delayMicroseconds(RTC_TICK_US);
        digitalWrite(PIN_RTC_CLK, HIGH);
        delayMicroseconds(RTC_TICK_US);
        digitalWrite(PIN_RTC_CLK, LOW);
        delayMicroseconds(RTC_TICK_US);
    }
    // The 8th falling edge above is what makes the chip start driving the line
    // on a read command. Release the line here rather than leaving it an
    // output: the DS1302 begins driving I/O immediately after that edge, and
    // holding a push-pull output even briefly means two drivers fighting over
    // one pad. The next rtc_write_byte() re-asserts OUTPUT itself.
    pinMode(PIN_RTC_DAT, INPUT);
}

static uint8_t rtc_read_byte() {
    uint8_t v = 0;
    pinMode(PIN_RTC_DAT, INPUT);
    for (uint8_t i = 0; i < 8; ++i) {
        delayMicroseconds(RTC_TICK_US);
        if (digitalRead(PIN_RTC_DAT)) v |= (uint8_t)(1u << i);
        digitalWrite(PIN_RTC_CLK, HIGH);
        delayMicroseconds(RTC_TICK_US);
        digitalWrite(PIN_RTC_CLK, LOW);     // next bit appears here
    }
    return v;
}

static uint8_t rtc_get(uint8_t reg) {
    rtc_begin_frame();
    rtc_write_byte(reg | 0x01);
    const uint8_t v = rtc_read_byte();
    rtc_end_frame();
    return v;
}

static void rtc_put_raw(uint8_t reg, uint8_t val) {
    rtc_begin_frame();
    rtc_write_byte(reg & 0xFE);
    rtc_write_byte(val);
    rtc_end_frame();
}

// Every write has to get past the write-protect bit. WP is re-armed afterwards
// so a stray clock edge can't silently corrupt the time.
static void rtc_put(uint8_t reg, uint8_t val) {
    rtc_put_raw(RTC_WP, 0x00);
    rtc_put_raw(reg, val);
    rtc_put_raw(RTC_WP, RTC_WP_BIT);
}

// ============================================================================
// BCD
// ============================================================================

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline bool bcd_ok(uint8_t v, uint8_t maxBin) {
    if ((v & 0x0F) > 9) return false;
    return bcd2bin(v) <= maxBin;
}

// ============================================================================
// Civil <-> epoch, UTC, independent of the TZ environment
//
// gmtime_r would do the epoch->civil direction safely, but the reverse needs
// either timegm() (not portable here) or mktime() (which honours TZ, a global
// this module has no business depending on). Howard Hinnant's days_from_civil
// is exact, branch-light, and leaves no global state to get wrong.
// ============================================================================

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2) ? 1 : 0;
    const int32_t  era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);                       // [0, 399]
    const uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        // [0, 146096]
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t rtc_time_to_epoch(const RtcTime* t) {
    const int32_t days = days_from_civil(2000 + (int32_t)t->year, t->month, t->date);
    return (uint32_t)days * 86400UL
         + (uint32_t)t->hour * 3600UL
         + (uint32_t)t->min * 60UL
         + (uint32_t)t->sec;
}

void rtc_epoch_to_time(uint32_t epoch, RtcTime* t) {
    const time_t tt = (time_t)epoch;
    struct tm g;
    gmtime_r(&tt, &g);                      // always UTC, TZ-independent
    t->sec   = (uint8_t)g.tm_sec;
    t->min   = (uint8_t)g.tm_min;
    t->hour  = (uint8_t)g.tm_hour;
    t->date  = (uint8_t)g.tm_mday;
    t->month = (uint8_t)(g.tm_mon + 1);
    t->year  = (uint8_t)(g.tm_year + 1900 - 2000);
    t->dow   = (uint8_t)(g.tm_wday + 1);    // tm_wday is 0=Sun; chip wants 1..7
}

// ============================================================================
// Raw chip access
// ============================================================================

// Reads the seven clock registers, then re-reads seconds and retries if it
// changed.
//
// The registers are read one frame at a time, so a rollover landing mid-read
// would otherwise produce a time that never existed — read seconds at 59, the
// minute ticks, read the new minute, and the result is a minute fast. Seconds
// is the fastest-changing field, so any rollover of a higher field implies
// seconds changed too: if seconds is the same before and after, the snapshot is
// coherent.
//
// (The DS1302 also has a clock-burst mode, 0xBF, which latches all eight
// registers atomically and would make this unnecessary. Deliberately not used:
// this is an SLM1302 clone and burst support is the kind of corner a clone
// skips. Re-reading costs one extra frame and depends on nothing.)
static bool rtc_read_raw_locked(RtcTime* t) {
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        const uint8_t sec = rtc_get(RTC_SEC);
        const uint8_t mn  = rtc_get(RTC_MIN);
        const uint8_t hr  = rtc_get(RTC_HOUR);
        const uint8_t dt  = rtc_get(RTC_DATE);
        const uint8_t mo  = rtc_get(RTC_MONTH);
        const uint8_t dw  = rtc_get(RTC_DOW);
        const uint8_t yr  = rtc_get(RTC_YEAR);

        if ((rtc_get(RTC_SEC) & 0x7F) != (sec & 0x7F)) continue;   // rolled over; redo

        t->sec   = bcd2bin((uint8_t)(sec & 0x7F));   // mask off CH
        t->min   = bcd2bin(mn);
        t->hour  = bcd2bin((uint8_t)(hr & 0x3F));
        t->date  = bcd2bin(dt);
        t->month = bcd2bin(mo);
        t->dow   = (uint8_t)(dw & 0x07);
        t->year  = bcd2bin(yr);

        // A missing chip reads as all 0x00 or all 0xFF; both fail here, as does
        // genuinely corrupt BCD.
        return bcd_ok((uint8_t)(sec & 0x7F), 59) && bcd_ok(mn, 59) &&
               bcd_ok((uint8_t)(hr & 0x3F), 23) &&
               bcd_ok(dt, 31) && bcd_ok(mo, 12) && bcd_ok(yr, 99) &&
               t->date >= 1 && t->month >= 1;
    }
    return false;   // three rollovers in a row means the bus is lying, not the clock
}

bool rtc_read_raw(RtcTime* t) {
    RtcLock lock;
    if (!lock.held) return false;
    return rtc_read_raw_locked(t);
}

static bool rtc_write_raw_locked(const RtcTime* t) {
    if (t->sec > 59 || t->min > 59 || t->hour > 23) return false;
    if (t->date < 1 || t->date > 31) return false;
    if (t->month < 1 || t->month > 12) return false;
    if (t->year > 99) return false;

    rtc_put_raw(RTC_WP, 0x00);
    // Seconds last would restart the clock mid-update, so write it first with
    // CH still set: the clock stays halted until every other field is in.
    rtc_put_raw(RTC_SEC,   (uint8_t)(bin2bcd(t->sec) | RTC_CH_BIT));
    rtc_put_raw(RTC_MIN,   bin2bcd(t->min));
    rtc_put_raw(RTC_HOUR,  bin2bcd(t->hour));      // bit7 clear = 24-hour
    rtc_put_raw(RTC_DATE,  bin2bcd(t->date));
    rtc_put_raw(RTC_MONTH, bin2bcd(t->month));
    rtc_put_raw(RTC_DOW,   (uint8_t)(t->dow ? t->dow : 1));
    rtc_put_raw(RTC_YEAR,  bin2bcd(t->year));
    rtc_put_raw(RTC_SEC,   bin2bcd(t->sec));       // CH cleared: start ticking
    rtc_put_raw(RTC_WP, RTC_WP_BIT);
    return true;
}

bool rtc_write_raw(const RtcTime* t) {
    RtcLock lock;
    if (!lock.held) return false;
    return rtc_write_raw_locked(t);
}

bool rtc_halted() {
    RtcLock lock;
    if (!lock.held) return true;    // can't tell -> assume the worst
    return (rtc_get(RTC_SEC) & RTC_CH_BIT) != 0;
}

uint8_t rtc_trickle_read() {
    RtcLock lock;
    if (!lock.held) return 0xFF;
    return rtc_get(RTC_TRICKLE);
}

// ============================================================================
// Time service
// ============================================================================

static bool rtc_valid_locked() {
    if ((rtc_get(RTC_SEC) & RTC_CH_BIT) != 0) return false;   // never set / halted
    RtcTime t;
    if (!rtc_read_raw_locked(&t)) return false;
    const uint32_t e = rtc_time_to_epoch(&t);
    return e >= RTC_EPOCH_MIN && e < RTC_EPOCH_MAX;
}

bool rtc_valid() {
    RtcLock lock;
    if (!lock.held) return false;
    return rtc_valid_locked();
}

uint32_t rtc_now() {
    if (!s_time_set) return 0;      // never established -> caller flags ts_unverified
    return (uint32_t)time(nullptr);
}

// Pushes an epoch into the system clock. Separated so rtc_init() and
// rtc_set_epoch() cannot drift apart.
static void system_clock_set(uint32_t epoch) {
    struct timeval tv;
    tv.tv_sec  = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    s_time_set = true;
}

bool rtc_set_epoch(uint32_t epoch) {
    if (epoch < RTC_EPOCH_MIN || epoch >= RTC_EPOCH_MAX) return false;

    RtcTime t;
    rtc_epoch_to_time(epoch, &t);

    RtcLock lock;
    if (!lock.held) return false;
    if (!rtc_write_raw_locked(&t)) return false;

    system_clock_set(epoch);
    s_last_wb_ms = millis();        // just wrote; no need for an immediate writeback
    return true;
}

void rtc_service() {
    if (!s_time_set) return;
    if ((uint32_t)(millis() - s_last_wb_ms) < RTC_WRITEBACK_INTERVAL_MS) return;
    s_last_wb_ms = millis();

    const uint32_t now = (uint32_t)time(nullptr);
    if (now < RTC_EPOCH_MIN || now >= RTC_EPOCH_MAX) return;   // don't push nonsense back

    RtcTime t;
    rtc_epoch_to_time(now, &t);

    RtcLock lock;
    if (!lock.held) return;         // busy; try again next hour
    rtc_write_raw_locked(&t);
}

// ============================================================================
// Init
// ============================================================================

void rtc_init() {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        Serial.println("WARN: RTC mutex creation failed — RTC disabled this boot");
        return;
    }

    // CE low BEFORE pinMode, same hazard out_safe_low() handles for the MOSFET
    // gates: IO25 floats at reset, and a CE that floats high can make the chip
    // see a frame that was never sent.
    digitalWrite(PIN_RTC_CE, LOW);
    pinMode(PIN_RTC_CE, OUTPUT);
    digitalWrite(PIN_RTC_CE, LOW);

    digitalWrite(PIN_RTC_CLK, LOW);
    pinMode(PIN_RTC_CLK, OUTPUT);
    digitalWrite(PIN_RTC_CLK, LOW);

    pinMode(PIN_RTC_DAT, INPUT);

    RtcLock lock;
    if (!lock.held) return;

    // Trickle charging off, unconditionally and every boot — see the warning in
    // rtc.h. BT1 is a CR2032 (confirmed 2026-09-14) and charging it can vent the
    // cell, so this is a safety step, not housekeeping.
    //
    // Written rather than assumed: the power-up default of an SLM1302 clone is
    // not something to bet a vented cell on. Then READ BACK, because a write
    // that silently failed — a miswired DAT line, a stuck WP — would leave the
    // cell charging with the firmware believing otherwise. "We wrote 0x00" and
    // "it is 0x00" are different claims and only the second one is worth having.
#ifdef RTC_ENABLE_TRICKLE_CHARGE
    rtc_put(RTC_TRICKLE, 0xA5);     // 1 diode, 2K series — LIR cells ONLY
#else
    rtc_put(RTC_TRICKLE, 0x00);
    const uint8_t tc = rtc_get(RTC_TRICKLE);
    if (tc == 0xFF) {
        // All-ones is the signature of a floating data line, i.e. no chip
        // responding — not a charging cell. Different fault, different message.
        Serial.println("WARN: RTC not responding (0x90 reads 0xFF) — chip absent or CE/CLK/DAT miswired");
    } else if (tc != 0x00) {
        Serial.print("*** DANGER: RTC trickle charger did NOT disable — 0x90 reads 0x");
        Serial.println(tc, HEX);
        Serial.println("*** BT1 is a CR2032 and may be being CHARGED. Power the board down");
        Serial.println("*** and remove the cell before investigating. See src/rtc.h.");
    }
#endif

    // Force 24-hour mode so read/write never deal with the AM/PM bit. Preserves
    // the current hour while clearing bit7.
    //
    // Guarded on the value being plausible 12-hour BCD: with no chip fitted IO4
    // floats and reads as noise, which would otherwise be "converted" and
    // written straight back as a garbage hour.
    const uint8_t h = rtc_get(RTC_HOUR);
    if (h & RTC_12H_BIT) {
        uint8_t hour12 = bcd2bin((uint8_t)(h & 0x1F));
        if ((h & 0x0F) <= 9 && hour12 >= 1 && hour12 <= 12) {
            const bool pm = (h & 0x20) != 0;
            if (hour12 == 12) hour12 = 0;               // 12 AM is hour 0
            const uint8_t hour24 = (uint8_t)(pm ? hour12 + 12 : hour12);
            rtc_put(RTC_HOUR, bin2bcd(hour24));
        }
        // Implausible: leave it alone. rtc_valid() will report it as untrusted,
        // and a set writes a clean 24-hour value anyway.
    }

    // Seed the system clock. This is the ONLY point at which the RTC is
    // authoritative — from here on time(nullptr) is the running clock.
    if (rtc_valid_locked()) {
        RtcTime t;
        if (rtc_read_raw_locked(&t)) {
            system_clock_set(rtc_time_to_epoch(&t));
        }
    }
}

// ============================================================================
// Reporting
// ============================================================================

static void print2(Stream& out, uint8_t v) {
    if (v < 10) out.print('0');
    out.print(v);
}

void rtc_print(Stream& out) {
    RtcTime t;
    const bool ok = rtc_read_raw(&t);

    if (!ok) {
        out.println("RTC read FAILED — registers are not valid BCD.");
        out.println("  Chip absent/unpowered, or CE/CLK/DAT miswired.");
        return;
    }

    out.print("rtc    = 20");
    print2(out, t.year);  out.print('-');
    print2(out, t.month); out.print('-');
    print2(out, t.date);  out.print(' ');
    print2(out, t.hour);  out.print(':');
    print2(out, t.min);   out.print(':');
    print2(out, t.sec);
    out.println(" UTC");

    out.print("epoch  = "); out.println((unsigned long)rtc_time_to_epoch(&t));

    out.print("clock  = ");
    out.println(rtc_halted() ? "*** HALTED (CH set) — never set, or stopped ***" : "running");

    out.print("status = ");
    if (rtc_valid()) {
        out.println("VALID — timestamps are trustworthy");
    } else {
        out.println("NOT TRUSTED — log entries will be flagged ts_unverified");
        out.println("  set it with  AT+RTC=<epoch>  or  AT+RTC=YYYY-MM-DD HH:MM:SS  (UTC)");
    }

    // System clock, which is what everything actually reads.
    if (s_time_set) {
        const uint32_t sys = (uint32_t)time(nullptr);
        RtcTime st;
        rtc_epoch_to_time(sys, &st);
        out.print("system = 20");
        print2(out, st.year);  out.print('-');
        print2(out, st.month); out.print('-');
        print2(out, st.date);  out.print(' ');
        print2(out, st.hour);  out.print(':');
        print2(out, st.min);   out.print(':');
        print2(out, st.sec);
        out.print(" UTC   (drift vs chip: ");
        out.print((long)sys - (long)rtc_time_to_epoch(&t));
        out.println(" s)");
    } else {
        out.println("system = NOT SET — rtc_now() returns 0 this boot");
    }

    const uint8_t tc = rtc_trickle_read();
    out.print("trickle(0x90) = 0x");
    out.print(tc, HEX);
    if (tc == 0x00) {
        out.println("  disabled (safe for a plain CR2032)");
    } else {
        out.println("  *** ENABLED — BT1 MUST be a rechargeable LIR cell ***");
    }
}
