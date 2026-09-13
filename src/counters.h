#pragma once

// ============================================================================
// Earnings counters — what the Live Counters characteristic (6a40f003) reports
//
// ESP32/S1 ONLY. No STM32 counterpart; intended divergence, recorded in
// docs/PORTING_FROM_STM32.md §2.5a.
//
// Four values, matching the app's decodeLiveCounters() byte-for-byte:
//
//     today_amount    u32  centavos taken since the business day began
//     today_sessions  u16  completed vends since the business day began
//     lifetime_amount u32  centavos taken, ever
//     last_seq        u32  highest event-log sequence number (B3; 0 until then)
//
// ---------------------------------------------------------------------------
// THE BUSINESS DAY IS ASIA/MANILA, NOT UTC. THIS IS NOT A PREFERENCE.
// ---------------------------------------------------------------------------
//
// The app displays today_amount *instead of* the backend's own DailyStat gross
// whenever a board is connected (DashboardScreen, AnalyticsScreen,
// MachineDetailScreen all do `hasLiveSnapshot ? liveCounters.todayAmount : ...`).
// So if this rolls over at a different instant than the backend's day, the
// number visibly jumps the moment an operator connects, and the two are never
// reconcilable.
//
// The app fixes that boundary in mobile/src/utils/businessDay.ts: Asia/Manila,
// a hard +08:00, deliberately NOT the phone's timezone, and the backend buckets
// DailyStat the same way. The Philippines has no DST, so a fixed offset is
// exact rather than an approximation.
//
// Manila midnight is therefore 16:00 UTC the previous day. That is what
// COUNTERS_TZ_OFFSET_MIN encodes. Do not "simplify" it to UTC midnight — in a
// UTC+8 market that files the first eight hours of trading under yesterday.
//
// ---------------------------------------------------------------------------
// LIFETIME MUST MOVE ON EVERY COIN, NOT ON EVERY SESSION
// ---------------------------------------------------------------------------
//
// The app's Diagnostics coin-path test takes a snapshot, asks the technician to
// drop a coin, and decides a coin was seen by `lifetimeAmount !== baseline ||
// todaySessions !== baseline` (DiagnosticsScreen.tsx). If lifetime only moved at
// session end, that test would report a dead coin path on a working board.
//
// This is the mechanism behind APP_BLE_PLAN A3 — the sanctioned way to confirm
// coin polarity (H1) without a serial console. So the increment goes in the coin
// task, at acceptance, not in the vend state machine.
//
// ---------------------------------------------------------------------------
// PERSISTENCE IS DEBOUNCED, DELIBERATELY
// ---------------------------------------------------------------------------
//
// Counters live in RAM and are flushed to NVS after COUNTERS_FLUSH_IDLE_MS of
// quiet, plus immediately on a day rollover. Writing per coin would be simpler
// and is wrong: a busy machine takes hundreds of coins a day, and NVS endurance
// is finite. Debouncing cuts writes by orders of magnitude at the cost of losing
// at most the last few seconds of activity to a hard reset.
//
// That trade is the same one REVIEW_FINDINGS R4 / APP_BLE_PLAN D3 are still open
// about for the customer's *banked* balance. This module deliberately does not
// pre-empt that decision — it covers the operator's earnings totals, which are
// reporting data, not money owed to someone standing at the machine.
//
// Like the serial, these live in their OWN NVS namespace, never in AppConfig:
// a factory reset must not zero a machine's lifetime takings, and an
// APP_CONFIG_MAGIC bump must not either. Standing rule from src/identity.h —
// identity and monotonic counters are not configuration.
// ============================================================================

#include <Arduino.h>
#include <stdint.h>

#define COUNTERS_NS  "vendocnt"
#define COUNTERS_KEY "totals"

// Asia/Manila. Fixed, exact, no DST. See the header comment before changing.
#define COUNTERS_TZ_OFFSET_MIN 480

// Quiet period before an NVS flush. Long enough to batch a burst of coins,
// short enough that a reset rarely costs more than one vend's worth.
#define COUNTERS_FLUSH_IDLE_MS 30000UL

// Load from NVS and settle the current business day. Call after rtc_init(), so
// the day can be established from a valid clock on the first try.
void counters_init();

// One accepted coin, worth `cents`. Called from the coin task at acceptance —
// see the lifetime note above. Safe from any task.
void counters_record_coin(uint32_t cents);

// One completed vend. Called from APP_STATE_SESSION_END.
void counters_record_session();

// Serialises the 14-byte Live Counters frame (little-endian) exactly as
// decodeLiveCounters() expects: today_amount u32 @0, today_sessions u16 @4,
// lifetime_amount u32 @6, last_seq u32 @10.
#define COUNTERS_WIRE_LEN 14
void counters_serialize(uint8_t out[COUNTERS_WIRE_LEN]);

// True if anything changed since the last call — lets the BLE layer notify only
// when there is news, rather than on a timer. Clears the flag.
bool counters_take_dirty();

// Day rollover check and the debounced NVS flush. Call from loop().
void counters_service();

// Individual accessors, for the CLI and for B3 later.
uint32_t counters_today_amount();
uint16_t counters_today_sessions();
uint32_t counters_lifetime_amount();
uint32_t counters_last_seq();

// The event log (B3) owns seq; this module only stores it so Live Counters can
// report it and so it survives a reboot in the same namespace as the totals.
void counters_set_last_seq(uint32_t seq);

// Business-day key (days since epoch, Manila-shifted) for a given UTC epoch.
// Exposed for testing and for B3's day bucketing.
uint32_t counters_business_day(uint32_t epoch_utc);

void counters_print(Stream& out);
