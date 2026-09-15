#pragma once

// ============================================================================
// Event log — the store behind BLE Session Log (6a40f004) and its delta sync
//
// ESP32/S1 ONLY. No STM32 counterpart; intended divergence, recorded in
// docs/PORTING_FROM_STM32.md §2.5a. Decision D7 in docs/APP_BLE_PLAN.md.
//
// ONE EVENT IS ONE SESSION — ONE PAID PERIOD, NOT ONE COIN PULSE
//
// Money in, relay runs, period ends: that whole thing is ONE row, carrying the
// total billed for it and timestamped when its first credit was billed. A
// customer who drops P20 at 10:00 and P100 at 10:05 into the same run produces
// one row of P120 at 10:00.
//
// **This is a deliberate divergence from the app team's reference firmware**,
// which calls addEvent() once per simulated coin (user's decision, 2026-09-15,
// after seeing both forms written out). Their reference is a bench rig where
// every simulated coin is P5 and drives one vend, so pulse == coin == vend and
// it never had to choose. On a real S1 the three come apart: at 1 pulse per
// peso — the normal acceptor wiring — P15 of coins is FIFTEEN rows carrying no
// information the one session row does not.
//
// The boundary is APP_STATE_SESSION_END, exactly where counters_record_session()
// already counts a vend, so today_sessions in Live Counters and the number of
// rows in this log now mean the same thing. (They deliberately did NOT before —
// see the note in src/counters.h, which is now historical.)
//
// WHAT THIS COSTS, STATED PLAINLY
//
// Money is recorded when it is BILLED, not when it enters the box. Coins sitting
// banked — inserted but not yet enough for a credit, or inserted and abandoned —
// are in the cash box and not yet in any row. They are not lost: whichever
// session eventually spends them carries them. The exception is a reset, which
// zeroes the banked balance (REVIEW_FINDINGS R4, open): that money is in the box
// and no row will ever account for it. Per-pulse logging did not have this hole.
// It is bounded by one credit's worth of change plus whatever R4 eats, and it
// was accepted knowingly.
//
// ---------------------------------------------------------------------------
// `denom` IS 0 ON EVERY ROW, AND THAT NEEDS THE APP TEAM'S SIGN-OFF
// ---------------------------------------------------------------------------
//
// The 14-byte frame was designed around coins: `denom:u8` is the coin's face
// value, and the app's AnalyticsScreen groups rows by it, counts "N coins" and
// draws a coin icon. A session has no denomination, so this firmware writes 0 —
// a value that cannot be mistaken for a real coin, which is the point. Same
// reasoning as the saturating totals in counters.cpp: an obviously wrong number
// gets noticed, a plausible one gets believed.
//
// Until the app filters `denom == 0` out of that breakdown, it will render a
// "P0 - 1 coin" row per session. Revenue totals stay correct either way, because
// those sum `amount`. See docs/APP_BLE_PLAN.md D18 for the note sent to them.
//
// ---------------------------------------------------------------------------
// THE WIRE IS PESOS. THE REST OF THE FIRMWARE IS CENTAVOS.
// ---------------------------------------------------------------------------
//
// Same boundary as Live Counters (D15): the billed total is accumulated in
// centavos — what app.cpp actually consumes — and divided by 100 exactly once,
// here, on the way into the record. `amount` is a u32, so a large session does
// not saturate. Price-per-pulse is always a whole number of pesos, so the
// division is exact; if someone configures a fractional price the log rounds
// DOWN and AT+LOG? says so, because there is no fractional field on this wire.
//
// ---------------------------------------------------------------------------
// STORAGE: A DEDICATED FLASH PARTITION, NOT NVS
// ---------------------------------------------------------------------------
//
// `vendolog`, 128 KB, carved out of the unused SPIFFS region — see
// partitions_vendo.csv for why nothing below 0x290000 was allowed to move.
//
//   32 sectors x 4096 B = 8192 slots of 16 B
//   ~8,100 sessions retained — months at any realistic vend rate. (Sized when a
//   row was one coin pulse, where it was ~8 days at P1/pulse. Left at 128 KB
//   rather than shrunk: the space is already carved out and unused otherwise.)
//
// The record is the 14 wire bytes plus a 2-byte checksum, in wire order, so
// serving a page is a memcpy and there is no second layout to keep in sync:
//
//   off 0  u32 seq              off 9  u8  denom   (always 0 — see above)
//   off 4  u32 ts (epoch UTC)   off 10 u32 amount  (PESOS billed)
//   off 8  u8  ts_unverified    off 14 u16 Fletcher-16 over bytes 0..13
//
// 16 bytes is also what the flash wants: esp_partition_write needs a 4-byte
// aligned offset and a length that is a multiple of 4. The two spare bytes buy
// torn-write detection for free.
//
// A partition holding bytes but no readable record is erased once at boot: that
// region was SPIFFS in the stock table and a repartition does not clear it, and
// flash writes can only clear bits — so writing into dirty slots would produce
// records that fail their own checksum, i.e. a log that silently stores nothing.
//
// **Wrap is oldest-first** (user's decision, 2026-09-15). Filling the ring
// erases the oldest 4 KB sector — 256 events — to make room. The wire format
// has no "you missed some" field and none was invented: the backend already
// stores `machine.lastSeq` and receives the first event's seq, so a gap is
// exactly `firstSeq > lastSeq + 1` and is derivable server-side with no
// protocol change. The oldest retained seq is printed by AT+LOG? for the bench.
//
// ---------------------------------------------------------------------------
// `seq` IS ASSIGNED AT THE FLASH WRITE, NOT AT SESSION END
// ---------------------------------------------------------------------------
//
// Two reasons, and the second is what makes reads fast:
//
//   * An event that never reaches flash (RAM queue overflow, a failed write)
//     must not consume a sequence number. A hole in the numbering would look to
//     the backend exactly like the buffer-overrun gap above — a real loss
//     reported as a false one is worse than either alone.
//   * Because every assigned seq lands in exactly one slot, and slots are
//     written strictly in order, seq -> slot is ARITHMETIC:
//
//         slot(seq) = (head + CAPACITY - (next_seq - seq)) % CAPACITY
//
//     A page read is then ~10 flash reads of 16 bytes instead of a scan of the
//     whole 128 KB partition. Each record read back is checked against the seq
//     that was asked for, so if the invariant is ever violated the page comes
//     up short rather than returning someone else's money.
//
// `seq` must be monotonic across reboots — that is the whole basis of
// idempotent delta sync. It is recovered from the flash scan at boot, and
// floored by counters_last_seq() (the `vendocnt` namespace) so that erasing the
// log cannot rewind it into ids the backend has already seen. Standing rule
// from src/identity.h: identity and monotonic counters are not configuration.
//
// ---------------------------------------------------------------------------
// NOTHING TOUCHES FLASH FROM THE VEND PATH
// ---------------------------------------------------------------------------
//
// The hazard here is not the watchdog — petting is unconditional in its own task
// (src/wdt.h), so a slow write does not reset the board. It is that a 40 ms
// sector erase inside the app task stalls the vend: the relay, the countdown and
// the button handling all live there, and SESSION_END is immediately followed by
// the idle screen the next customer looks at.
//
// So eventlog_record_session() only posts to a 32-deep FreeRTOS queue and
// returns; eventlog_service() drains it to flash from loop(), alongside
// counters_service(). Writes are immediate rather than debounced the way
// counters are — a lost counter tick is a rounding error in a total, a lost
// SESSION is a permanent hole in the backend's history.
// ============================================================================

#include <Arduino.h>
#include <stdint.h>

// One record on flash: the 14 wire bytes + a 2-byte checksum.
#define EVENTLOG_WIRE_LEN    14
#define EVENTLOG_RECORD_LEN  16

// Session Log page: count:u8 | has_more:u8 | count x 14-byte events.
//
// 10 events = 142 bytes. Larger than the default 23-byte ATT MTU on purpose —
// the app requests MTU 247 and falls back to ATT long reads otherwise, which
// works because a page is a pure function of its cursor (ble_sessionlog.cpp
// caches it per connection so a coin arriving between blob segments cannot
// change the value mid-read).
//
// The app team's bring-up firmware uses 1 event per page; its own comment calls
// that bring-up caution rather than a contract requirement. At 1 event per
// round trip, draining a full ring would be 8,000 write+read exchanges.
#define EVENTLOG_PAGE_MAX_EVENTS 10
#define EVENTLOG_PAGE_MAX_LEN    (2 + EVENTLOG_WIRE_LEN * EVENTLOG_PAGE_MAX_EVENTS)

// Finds the `vendolog` partition, scans it, and recovers {oldest, newest, head}.
// Call AFTER counters_init() — counters_last_seq() is the floor that stops an
// erased log from rewinding seq — and BEFORE ble_config_init(), which registers
// the characteristic that reads it.
void eventlog_init();

// True if the partition was found and the log is usable. False on a board
// flashed with a partition table that has no `vendolog` (i.e. the stock
// default.csv) — everything else then degrades to a log that records nothing,
// rather than to a crash.
bool eventlog_ready();

// Adds `cents` to the session currently being billed, and — on the first call
// after a session was closed — stamps that session with the time NOW. Called
// from coin_consume_value_cents(), so every billing path reaches it: the initial
// credit block in APP_STATE_COIN_VALIDATE, the per-credit gated top-ups, and
// session.cpp's accumulation. Touches neither flash nor a queue.
//
// The timestamp comes from rtc_now() (time() plus a has-been-set flag), never
// rtc_valid(), which bit-bangs the SLM1302 and takes its mutex — src/rtc.h is
// explicit that a hardware read has no business in the vend path.
//
// APP TASK ONLY. The accumulator is deliberately unlocked: all three billing
// call sites and eventlog_record_session() run in the app task, and taking the
// module mutex here would let a flash write in loop() block the vend path for
// the length of a sector erase.
void eventlog_note_billed(uint32_t cents);

// Closes the session being billed and queues it as one row. Call from
// APP_STATE_SESSION_END, next to counters_record_session(). Does nothing if
// nothing was billed. App task only; posts to the queue and returns.
void eventlog_record_session();

// Drains the queue to flash. Call from loop(), next to counters_service().
void eventlog_service();

// Builds one Session Log page into `out` (must be EVENTLOG_PAGE_MAX_LEN).
// `after_seq` is EXCLUSIVE — events with seq strictly greater, which the app
// relies on for EVERY page, not just the first: it advances its cursor to the
// last seq it received, not that plus one. Returns the byte length.
size_t eventlog_build_page(uint32_t after_seq, uint8_t* out);

// Highest seq written so far (0 if the log is empty). This is what Live
// Counters reports as last_seq, and what the app sends back as sync_ack when a
// pull returned no events.
uint32_t eventlog_last_seq();

// Oldest seq still retained. The difference between this and the backend's
// lastSeq is how many events a long-offline client lost to the wrap.
uint32_t eventlog_oldest_seq();

// Sessions currently on flash.
uint32_t eventlog_count();

void eventlog_print(Stream& out);                                       // AT+LOG?
void eventlog_dump(Stream& out, uint32_t after_seq, uint32_t max_rows);  // AT+LOG=<n>
