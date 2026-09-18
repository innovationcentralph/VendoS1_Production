#pragma once

// ============================================================================
// Earnings counters — what the Live Counters characteristic (6a40f003) reports
//
// ESP32/S1 ONLY. No STM32 counterpart; intended divergence, recorded in
// docs/PORTING_FROM_STM32.md §2.5a.
//
// Four values, matching the app's decodeLiveCounters() byte-for-byte:
//
//     today_amount    u32  PESOS taken since the business day began
//     today_sessions  u16  VENDS delivered since the business day began
//     lifetime_amount u32  PESOS taken, ever
//     last_seq        u32  highest event-log sequence number (B3; 0 until then)
//
// ---------------------------------------------------------------------------
// THE WIRE IS PESOS. INTERNALLY THIS MODULE IS CENTAVOS.
// ---------------------------------------------------------------------------
//
// Firmware money is centavos everywhere else — `price_per_credit_cents` is the
// Config contract's own field name, and 1000 there means P10.00. But the app
// renders these two figures with NO division by 100 (`formatCurrency` is just
// `P${amount}`), and the reference firmware the app was built against
// accumulates whole pesos (`lifetimeAmount += denom`, denom = 5 for a P5 coin).
//
// So the conversion happens at the wire boundary, in counters_serialize(), and
// nowhere else. Get this wrong in the other direction and every machine reports
// earning 100x its real take.
//
// It is exact, not lossy: price-per-pulse is always a whole number of pesos, so
// every accumulated total is a whole multiple of 100 centavos. The accessors
// below stay in centavos so the CLI can print exact pesos-and-centavos.
//
// (Worth raising with the app team: the contract is internally inconsistent —
// Config carries centavos, Live Counters and Session Log carry pesos.)
//
// ---------------------------------------------------------------------------
// A SESSION IS ONE PAID PERIOD — A VEND. THIS DIVERGES FROM THE REFERENCE.
// ---------------------------------------------------------------------------
//
// A session is what a customer bought: money in, relay engages, machine runs,
// period ends. Counted once at APP_STATE_SESSION_END.
//
// **The deciding argument is non-redundancy.** today_amount already carries the
// money, and credits are just money / coins_required — so counting credits here
// would make this field a near-duplicate of one the app already has. Paid
// periods report something nothing else does: how many customers the machine
// served. Two fields, two questions.
//
// (Credits were the serious alternative, and they have one attractive property:
// today_amount / today_sessions would always equal coins_required exactly, a
// free misconfiguration check. Rejected anyway, for the redundancy above.)
//
// **Not** the number of relay transitions. In OP_PAUSE_RESUME the relay toggles
// several times inside one paid period, so counting GPIO edges would report one
// customer as three or four sessions. SESSION_END fires once per paid period
// however many times the relay cycled within it.
//
// **Not** the pulse count either — and that IS what the app's reference
// firmware does (`todaySessions++` inside addEvent(), the per-coin path).
// Deliberate divergence, on the product owner's call, because that reference is
// a demo rig where every simulated coin is P5, produces one event, and drives
// one vend: pulse == coin == vend, so the three meanings collapse and it never
// had to choose. On a real S1 they diverge violently — a P10 coin at P1/pulse
// is 10 pulses, 1 coin, 1 vend.
//
// Reporting pulses here would put "Sessions today: 10" on the operator's screen
// for one customer. The label would be a lie and the number useless.
//
// The app's Diagnostics coin-path test keys off `lifetimeAmount !== baseline ||
// todaySessions !== baseline`, and lifetime still moves on every pulse, so that
// test does not depend on this field.
//
// ⚠️ UPDATED 2026-09-15: the event log now records ONE ROW PER SESSION as well
// (user's decision — see src/eventlog.h), so this counter and the number of
// rows in the Session Log finally agree on what a session is. The earlier
// warning here — that the two meant different things — no longer applies. What
// DOES follow from that change: the app's per-denomination Analytics breakdown
// is built from Session Log rows, and those rows no longer carry a coin
// denomination. See D18 in docs/APP_BLE_PLAN.md.
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

// One accepted pulse, worth `cents`. Called from the coin task at acceptance —
// see the lifetime note above. Moves the money totals only; the session count
// is a separate event. Safe from any task.
void counters_record_coin(uint32_t cents);

// One completed vend — one paid period. Called from APP_STATE_SESSION_END.
// See the session definition above before moving this call site.
void counters_record_session();

// TEST MODE only: one pulse accepted while coin_set_test_capture() is on, worth
// `cents`. Moves ONLY the test total below - never today/lifetime, never NVS.
// A test coin is the technician's, not the operator's takings.
void counters_record_test_coin(uint32_t cents);

// Zero the test total. The app task calls this on test-mode entry, on every
// coin-slot enable inside test mode, and on exit, so a stale total can never
// be read as a coin from the current run.
void counters_reset_test_amount();

// Serialises the 18-byte Live Counters frame (little-endian): today_amount u32
// @0, today_sessions u16 @4, lifetime_amount u32 @6, last_seq u32 @10,
// test_amount u32 @14 (PESOS, RAM-only, 0 outside test mode).
//
// test_amount is APPENDED, never inserted: the app's decodeLiveCounters() reads
// fixed offsets and ignores trailing bytes, so an app build from before the
// field existed decodes the first 14 bytes exactly as before.
#define COUNTERS_WIRE_LEN 18
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
