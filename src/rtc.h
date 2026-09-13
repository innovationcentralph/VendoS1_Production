#pragma once

// ============================================================================
// U6 — SOLA-IC SLM1302, a DS1302-family 3-wire RTC on IO25/IO26/IO4.
//
// ESP32/S1 ONLY. The STM32 board has no RTC, so nothing was ported from
// upstream — this is a port of the *bring-up harness* driver
// (Vendo_S1_TestCode/src/s1_rtc.cpp), which was written against the DS1302
// datasheet and proven on this board. See docs/PORTING_FROM_STM32.md §2.5a.
//
// WHY THERE IS A CLOCK HERE AT ALL
//
// The event log (docs/APP_BLE_PLAN.md B3) records {seq, ts, denom, amount}.
// Without a real `ts` the backend can order events but not place them, so
// there is no DailyStat, no revenue-by-day, no correlating a fault with a
// visit. millis() cannot help: it restarts at zero every boot. The ESP32's
// internal RTC survives deep sleep but NOT a power cut, and a vending machine
// loses power routinely. BT1 keeps the SLM1302 die powered across that.
//
// THE DIVISION OF LABOUR — read this before changing anything
//
// **The SLM1302 is a power-loss backup, not the running clock.** While the
// board is powered, the ESP32 keeps better time than the RTC does: its 40 MHz
// crystal is typically ±10 ppm, against an untrimmed 32.768 kHz watch crystal
// that is usually worse. So:
//
//   boot        -> read the RTC once, settimeofday()
//   running     -> time(nullptr). Cheap. The event log calls THIS, never the
//                  RTC — a hardware read is ~350 us of bit-banging and has no
//                  business in a per-coin path.
//   on a set    -> write BOTH the RTC and the system clock
//   hourly      -> write system -> RTC, so a power cut loses as little as
//                  possible (rtc_service(), called from loop())
//
// So the RTC is read once per boot and written rarely, and its own drift only
// matters across an outage.
//
// TIME IS UTC. Not a local-time offset in sight. The app's Time Sync
// characteristic (6a40f002) sends `epoch_utc:u32`, and the backend converts for
// display. PH is UTC+8 with no DST, so if scheduled operation is ever wanted it
// can add an offset field then — don't pre-build it.
//
// ⚠️ TRICKLE CHARGING MUST STAY OFF ON THIS BOARD. BT1 IS A CR2032.
//
// **Confirmed 2026-09-14: BT1 is a plain CR2032 — NON-RECHARGEABLE.**
//
// The DS1302's register 0x90 pushes charge current back into the backup cell.
// That is correct for a rechargeable LIR-series cell. Into a CR2032 it is not a
// bug, it is a hazard: the cell heats, swells, and can vent or rupture. This is
// a safety property of the firmware, not a configuration preference.
//
// So rtc_init() writes 0x00 every boot rather than trusting whatever state the
// part powered up in, and then READS IT BACK and complains loudly if it did not
// take. Do not remove either half.
//
// The RTC_ENABLE_TRICKLE_CHARGE hook below survives only for a hypothetical
// future board revision fitted with an LIR cell. On THIS board there is no
// circumstance in which defining it is correct — which is why defining it alone
// is a compile error (see the guard under it).
// ============================================================================

#include <Arduino.h>
#include <stdint.h>
#include <time.h>

// Uncomment ONLY on a board revision confirmed to carry a rechargeable
// LIR-series cell. NOT this one — BT1 is a CR2032 (confirmed 2026-09-14).
// #define RTC_ENABLE_TRICKLE_CHARGE

// Deliberately two acts, not one. Enabling trickle charge into the wrong cell
// is a venting hazard, so it must not be reachable by uncommenting a single
// line that happens to sit next to an explanation someone skimmed.
#if defined(RTC_ENABLE_TRICKLE_CHARGE) && !defined(RTC_BT1_IS_RECHARGEABLE_LIR)
#error "RTC_ENABLE_TRICKLE_CHARGE requires RTC_BT1_IS_RECHARGEABLE_LIR. BT1 on the Vendo S1 is a CR2032 (non-rechargeable) and trickle charging it can vent the cell. Do not define either macro unless the board in front of you is physically fitted with an LIR cell."
#endif

// Accepted/valid window for any timestamp, as epoch seconds UTC.
//
// A clamp rather than a nicety: the serial and the event log both survive
// reboots, and a client that sets 1970 (a common uninitialised-value bug) would
// put log entries permanently before every real one and corrupt ordering that
// delta sync depends on. Same spirit as the config validators. Doubles as the
// sanity half of rtc_valid().
#define RTC_EPOCH_MIN 1767225600UL   // 2026-01-01T00:00:00Z
#define RTC_EPOCH_MAX 2524608000UL   // 2050-01-01T00:00:00Z

// Broken-down time as the chip stores it. Field names match the harness's
// S1Time so the two stay diffable.
struct RtcTime {
    uint8_t sec;    // 0..59
    uint8_t min;    // 0..59
    uint8_t hour;   // 0..23 — 24-hour mode is forced at init
    uint8_t date;   // 1..31
    uint8_t month;  // 1..12
    uint8_t dow;    // 1..7, 1 = Sunday
    uint8_t year;   // 0..99, offset from 2000
};

// Pins, trickle-charge off, 24-hour mode, then read the chip once and seed the
// system clock. Safe to call before the scheduler has other tasks running; it
// creates its own mutex first. ~350 us of bit-banging, nowhere near any
// watchdog budget.
void rtc_init();

// True if the chip holds a time we are willing to believe: the clock is running
// (CH clear) AND the date falls inside [RTC_EPOCH_MIN, RTC_EPOCH_MAX).
//
// Both halves are needed. CH alone catches a chip that has never been set, but
// a board that sat with a flat cell can come back with CH clear and garbage in
// the registers — which would otherwise present as a confident, wrong
// timestamp. A date outside the window means "do not trust", not "clamp".
bool rtc_valid();

// Current time as epoch seconds UTC, from the SYSTEM clock (see the division of
// labour above). Returns 0 if time has never been established this boot, which
// is what the event log's `ts_unverified` flag keys off — log the event, flag
// the timestamp, never guess.
uint32_t rtc_now();

// Set the time from epoch seconds UTC. Writes the RTC *and* the system clock.
// Rejects anything outside [RTC_EPOCH_MIN, RTC_EPOCH_MAX). This is the one
// entry point for both AT+RTC= and the BLE Time Sync characteristic, so the
// clamp cannot be bypassed by adding a second writer.
bool rtc_set_epoch(uint32_t epoch);

// Periodic housekeeping — call from loop(). Writes the system clock back to the
// RTC once an hour so a power cut loses at most that hour's drift. Cheap: a
// millis() compare on every call, a write once an hour.
void rtc_service();

// Raw chip access. Prefer rtc_now()/rtc_set_epoch(); these exist for
// diagnostics and for the conversion layer.
bool rtc_read_raw(RtcTime* t);          // false if the registers aren't valid BCD
bool rtc_write_raw(const RtcTime* t);   // also clears CH so the clock runs
bool rtc_halted();                      // CH bit — a new chip ships halted
uint8_t rtc_trickle_read();             // register 0x90; 0x00 = disabled

// Conversion helpers, UTC, no dependence on the TZ environment.
uint32_t rtc_time_to_epoch(const RtcTime* t);
void     rtc_epoch_to_time(uint32_t epoch, RtcTime* t);

// Human-readable status for AT+RTC? and the boot banner.
void rtc_print(Stream& out);
