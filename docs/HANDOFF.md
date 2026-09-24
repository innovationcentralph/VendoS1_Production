# Session handoff — next task: prove the vend path

**Read `CLAUDE.md` first** (repo conventions, watchdog hazards, money invariants,
the diffability rule). The live backlog is `docs/APP_BLE_PLAN.md` — that file is
maintained; this one is a snapshot and will go stale.

---

# DONE THIS SESSION: `f004` Session Log

`D7` was decided and `B3` built. Compiles clean in both environments;
**nothing has been run on hardware.**

| File | What |
|---|---|
| `partitions_vendo.csv` | `vendolog`, 128 KB, carved from the unused SPIFFS region |
| `src/eventlog.h/.cpp` | The event store: ring of 8,192 x 16-byte rows, oldest-first wrap. **One row = one session** |
| `src/ble_sessionlog.h/.cpp` | `6a40f004`, write-cursor-then-read pagination, cursor per connection |
| `tools/eventlog_ring_model.py` | Python transliteration of the ring arithmetic; 18 checks, all passing |
| `src/periph.cpp` | `eventlog_note_billed()` inside `coin_consume_value_cents()` — the one point every billing path passes through |
| `src/main.cpp` | `eventlog_init()` after `counters_init()`; `eventlog_service()` in `loop()` |
| `src/app.cpp` | `eventlog_record_session()` at `APP_STATE_SESSION_END`, next to `counters_record_session()` |
| `src/cli.cpp` | `AT+LOG?`, `AT+LOG=<rows>[,<after_seq>]` |

**`D7` as decided:** 128 KB dedicated flash partition, ~8,100 rows, wrapping
oldest-first, 10 rows per page. NVS was rejected — it is 20 KB in total and holds
the write-once factory serial. The depth was sized when a row was one coin pulse
(~8 days at P1/pulse); now that a row is a session it is months, and was left as
it is because the space is carved out and otherwise unused.

**`D17`, the "you missed rows" question:** nothing was added to the wire. The
backend already stores `machine.lastSeq` and receives the first event's `seq`, so
a wrap gap is `firstSeq > lastSeq + 1`, derivable server-side. `AT+LOG?` prints
the oldest retained seq. **Tell the app team** — nothing acts on it today.

**`D18`, granularity — changed late in the session on the user's call.** A row is
**one session** (one paid period), not one coin pulse. The per-pulse form was
built first; comparing the two settled it. Consequences, all recorded in
`APP_BLE_PLAN` `D18`/`D19`:

- `denom` is **0** on every row — a session has no coin face value. **The app
  must exclude `denom == 0` from its coin breakdown**, or it renders "P0 — 1
  coin" per session. The ask is written out in `D18`, ready to send.
- `denom` was never reachable on this hardware anyway: it was
  `price_per_credit_cents / 100`, a config value identical on every row.
- Money is logged when **billed**, not when it enters the box. Banked coins are
  carried by the session that spends them — unless a reset eats them first
  (`R4`). Accepted knowingly; bounded by one credit's worth of change.

### Four things a reviewer should check first

1. **`seq` is assigned at the flash write, not at session end.** A row that never
   reaches flash must not consume a number, or the hole looks exactly like the
   wrap gap above. This is also what makes `seq -> slot` pure arithmetic and a
   page ~10 reads instead of a 128 KB scan.
2. **Nothing touches flash from the vend path.** The hazard is *not* the watchdog
   — petting is unconditional in its own task. It is that a 40 ms sector erase in
   the app task stalls the relay, the countdown and the buttons.
   `eventlog_record_session()` queues; `eventlog_service()` writes from `loop()`.
   The billing accumulator is deliberately unlocked — all its writers and its
   one reader are the app task, and taking the module mutex there would let a
   flash write in `loop()` block a vend.
3. **The cursor is per BLE connection**, released in the new server
   `onDisconnect`. One global cursor lets two phones corrupt each other's
   pagination.
4. **A page is built when the cursor is written**, so an ATT long read at the
   default 23-byte MTU collects a stable buffer.

---

# ALSO DONE: `f007` Command — the BLE bundle is complete

`src/ble_command.h/.cpp`, all seven ops, plus `AT+SYNC?`. Compiles clean,
**never run on hardware.**

| | |
|---|---|
| `src/ble_command.h/.cpp` | `6a40f007`, Write. Mailbox + executor |
| `src/app.h/.cpp` | `app_state_is_idle()` — one published word, one line in the state machine |
| `src/main.cpp` | `ble_command_service()` in `loop()` |
| `src/cli.cpp` | `AT+SYNC?` |

Three things a reviewer should check:

1. **The GATT callback is a mailbox, not an executor.** `buzzer_beep_*()` blocks
   for hundreds of ms; doing that in a write callback stalls the NimBLE host
   task, including the connection awaiting the ATT response to that write. The
   relay is worse — it belongs to the app task, and two owners on one GPIO is how
   a relay ends up latched with nobody responsible.
2. **Physical ops are refused while vending**, and **dropped rather than
   deferred**. A beep replayed minutes later, after the operator stopped
   watching, is worse than no beep. `sync_ack`/`clear_errors`/`wifi_forget` are
   ungated on purpose: refusing a `sync_ack` mid-vend fails the app's connect for
   nothing.
3. **`sync_ack` is recorded, never applied.** Its param is the *backend's* last
   seq and can be lower than the board's. Writing it into
   `counters_set_last_seq()` would rewind the numbers `f004` exists to protect.

**`reboot` was not invented** — `0x01`–`0x07` are all allocated and `0x08` is the
app team's to assign. `A4` stays open.

---

# f001 IS ON (2026-09-23)

`f001` Device Info is always built — the flag and the `esp32dev-devinfo` env are
gone, and an S1 always reads as `full`. Open app-side consequence, **`A7`**: the
app asserts `deviceInfo.deviceId === machine.deviceId` on every `full` connect,
and a machine claimed while the board was configOnly stored the BLE MAC there, so
it fails with "Connected to the wrong board" until re-claimed.

---

# THE TASK: prove the vend path on hardware

The BLE surface is now six characteristics and the board still has never
completed a vend under test. That is the whole of `H2`, and it outranks
everything left on the BLE side.

Nothing in the vend state machine has ever executed on a board: no operation
mode, no per-credit gate, no accumulation path, no session expiry, no sensor
stop. It is a faithful port of firmware that works in the field — which is a
reason to expect it to work, not evidence that it does.

**Start here, because the first three need no new code:**

1. **`R1`** — `price_per_credit_cents` can be set to 0, which banks P0.00 per
   coin. The app already clamps client-side and cites the finding by name; the
   board should reject it too.
2. **`R2`** — Sensor Stop has no arming requirement, so a stuck sensor ends every
   session in ~60 ms.
3. **`R3`** — `blocks * relay_on_ms` has no clamp; `uint32_t` overflow can
   *shorten* a paid session.
4. **`R4`** — banked money is RAM-only. Now sharper than it was: with session-
   granularity logging, money banked and lost to a reset appears in no row at
   all (see `src/eventlog.h`).

Then run each operation mode on a board with a meter on the relay.

---

## Invariants that must not be broken

- **Do not add `wdt_pet()` anywhere.** It races the wdt task on IO2.
- **Nothing blocking in the coin task** — it is a 5 ms poll and blocking it loses
  money, silently.
- **Nothing below `0x290000` in `partitions_vendo.csv` may move.** `nvs` at
  `0x9000` holds the write-once serial.
- **`f001` stays gated** until the app has the `A7` re-claim path. `f007` now
  exists, so `A7` is the only thing left holding it — and it is app-side.

---

## Build state

```
pio run -t upload                      # esp32dev — f001 always built, app sees 'full'
```

⚠️ **This build changes the partition table.** `-t upload` flashes
`partitions.bin` alongside the firmware, so a normal upload is enough — but a
board flashed with only `firmware.bin` keeps its old table and prints
`` `vendolog` partition not found — Session Log (f004) DISABLED `` at boot.
`nvs` does not move, so the factory serial and the earnings totals survive.

Flash ~50.5%, ~662 KB of 1.31 MB. RAM 11.1%. Headroom is not a constraint.

---

## Verified on hardware vs merely compiled

**Proven on a board:** BLE Config read/write · `AT+SERIAL=` / `AT+SERIAL?` ·
`f001` Device Info (byte-exact 38-byte frame) · `f003` Live Counters · `f006`
Diagnostics (correct 13-byte frame, and it caught a real fault — below) · coin
counting.

**Built, never exercised:** `f002` over BLE · **all of `f004`, including the
first flash write to the new partition** · RTC across a power cut (**the test
that matters**) · serial write-once rejection · `AT+SERIAL_ERASE` · whether NVS
survives a reflash · **the entire vend path**.

`tools/eventlog_ring_model.py` exercises the ring's *arithmetic* (wrap, the
exclusive cursor, the app's pagination loop, the peso conversion) because there
is no host toolchain here. It says nothing about whether the C++ matches it, or
about flash.

**Zero of the 22 `REVIEW_FINDINGS` are closed.** All four HIGH money findings
stand: `R1` price can be 0, `R2` a stuck sensor ends every session in ~60 ms,
`R3` overflow can shorten a paid session, `R4` any reset zeroes the customer's
balance.

---

## Open bench items — one board closes several

```
AT+COIN_POLARITY=1     # then power-cycle
AT+COIN?               # pulses must read 0, not 1           -> closes H1
AT+CFG?                # confirm price_per_credit_cents != 0  -> R1 check
                       # meter COIN_IN to GND, no coin:
                       #   <0.4V healthy | 0.5-2.0V = H5, raise R25 to 4.7K
AT+LOG?                # must say the vendolog partition was FOUND
                       # drop a coin: AT+LOG? count must NOT move yet,
                       # and "open = Pxx" must appear (billed, session not ended)
                       # press START, let the session finish, then:
AT+LOG=5,0             # ONE row for the whole session, amount in PESOS, denom 0
                       #                                  -> first f004 proof
AT+RTC=2026-09-14 12:00:00
                       # pull power 60s, boot
AT+RTC?                # must still be correct                -> closes B2's gap
AT+LOG?                # seq range must NOT have rewound      -> proves the seq floor
AT+SERIAL=VLABS-S1-00002   # must be REJECTED (write-once)
AT+SYNC?                   # "never received" until an app actually connects
```

**`f007` bench checks (nRF Connect, characteristic `6a40f007`, 5-byte writes):**

```
02 00000000   clear_errors  -> AT+DIAG? list empties (0x03 re-raises if clock unset)
03 00000000   identify      -> beep + LED
05 00000000   test_buzzer   -> start-beep pattern (2x 1000 Hz)
06 00000000   test_led      -> D7 blue DEBUG led, 3x 200 ms
08 00000000   test_user_led -> J1 BUTTON LAMP, 5x 100 ms (provisional op, D21/A13)
01 2A000000   sync_ack(42)  -> AT+SYNC? shows seq 42
04 00000000   test_relay    -> relay clicks for 500 ms
```

**Test mode (`0x09`/`0x0A`, provisional — `D22`/`D23`):**

```
09 01000000   enter test mode   -> LCD "TEST MODE / SLOT INHIBITED", vend dead
0A 01000000   coin slot on      -> LCD "TEST MODE / COINS: n", counts from 0
              drop a coin       -> n increments; AT+TEST? shows it too
0A 00000000   coin slot off     -> "PLS:-- BTN:n"
              press the button  -> BTN:n increments with NO command needed
0B 00000000   reset btn count   -> BTN:0
09 00000000   leave test mode   -> idle screen returns
```

The J1 harness is worth doing as one pass, since both its pins are on the same
connector: `08` flashes the lamp (J1 pin 2), pressing the button moves `BTN:`
(J1 pin 1). A lamp that flashes with a button that never counts is a broken
pin 1; neither working points at the connector or Q1.

### Watching the coin and button over BLE, in nRF Connect

The default build includes the user-button bit (`-DENABLE_DIAG_BUTTON_BIT`;
allocation still pending, `A14`).

Connect to `VLABS-S1-<serial>` (or `VLABS-UNSET-xxxx`), service
`6a400001-0000-1000-8000-00805f9b0001` — **not** `6a40f000`, which is the
app-repo bring-up firmware. Enable CCCD (the three-arrows icon) on `f003` and
`f006` first; `notify()` is a no-op with no subscriber. Write ops to `f007` as a
**BYTE ARRAY**, 5 bytes, **Write Request** — the characteristic is `WRITE`, so
write-without-response is rejected.

**Coin — `f003` notifies once per pulse**, 14 bytes, all little-endian:

| Offset | Len | Field | Note |
|---|---|---|---|
| 0 | 4 | `today_amount` | **pesos**, not centavos |
| 4 | 2 | `today_sessions` | u16 — leaves the next field on an odd offset, which is the contract |
| 6 | 4 | `lifetime_amount` | pesos |
| 10 | 4 | `last_seq` | |

Drop a coin and watch bytes 0-3 and 6-9 step. `pulses = Δamount / (price_per_credit_cents / 100)`.

**Button — `f006` notifies on each edge**, so two frames per press:

| Offset | Len | Field |
|---|---|---|
| 0 | 4 | `uptime_s` |
| 4 | 4 | `boot_ts` |
| 8 | 1 | `boot_ts_unverified` |
| 9 | 1 | **`sensors_bitmap`** |
| 10 | 1 | `errors_count` |
| 11+ | n | fault codes |

Byte 9: `0x01` door/AUX_1, `0x02` vibration (always 0 on the S1), `0x04` coin line
asserted now, `0x08` RTC valid, `0x10` **user button pressed**. With the clock set,
byte 9 should read `08` -> `18` on press -> `08` on release. If `0x10` never
appears you are on the wrong build.

Then prove the three exits, because a board that cannot leave test mode is a
board out of service in the field:

```
09 01000000   then disconnect the phone and wait   -> exits itself after 60 s
09 01000000   then hold BTN1+BTN2 long             -> exits, no phone needed
09 01000000   then tap RESET                       -> boots in NORMAL operation
```

Also confirm `09 01000000` is **REFUSED** during a session (it must never
abandon a paid vend), and that `0A` is refused when not in test mode.

`03` and `08` are the two that were wrong or missing before 2026-09-17, so check
them deliberately: `identify` used to call an empty beep stub (silent, with an
LED pulse microseconds wide), and the button lamp had no test at all — `06` only
ever drove D7, which no customer looks at. `08` is executed by the **app task**,
not `loop()`, because that task re-asserts IO23 every 20 ms in IDLE; expect
`[app] USER_LED test` on serial, not just the `BLE test_user_led` line.

Then start a session and send `04` again **while it runs**: it must be REFUSED,
with `test_relay REFUSED — a session is in progress` on the serial console and
no relay click. That gate is the one part of `f007` worth testing carefully —
everything else is bookkeeping.

### Live finding from 2026-09-14, unresolved

A board reported `f006` error `0x01` (coin line fault) **while the coin slot
appeared to work**. Diagnosis: the stored config had `coin_active_high = false`
while the hardware idles LOW, so the line read as permanently asserted — one
phantom credit at power-on, then counting on the *trailing* edge. Looked fine
from outside.

**The general hazard this exposed:** `kDefaults` changes only reach boards with
**no stored config**. Commit `5fd868c` fixed the polarity default and did not
reach an already-provisioned board, and will not reach any board in the field.
Belongs in the bench and factory checklists.

---

## Honest priority note

Unchanged from the last session, and now one characteristic more true. **The
board's ability to correctly take money and deliver a vend still has not moved.**

`H2` (prove the vend path — no operation mode, per-credit gate or accumulation
path has ever executed on hardware) and `M1`–`M4` (the money findings) were worth
more than any of the BLE work, and they still are. The 2026-09-14 polarity
finding is that argument in miniature: a real misconfiguration that looked like
it worked, found only because something finally looked.

**The BLE excuse is now gone.** Six characteristics exist, one of them verified
byte-exact on hardware, and the remaining app-side blockers are not firmware's to
clear. There is nothing left on that workstream to do instead of proving the
vend path — which is why it is this document's task.

---

## Docs map

| | |
|---|---|
| `CLAUDE.md` | conventions, invariants, hazards — read first |
| `docs/APP_BLE_PLAN.md` | **the live backlog**, stable IDs, every decision with reasoning |
| `docs/BLE_CONFIG_CONTRACT.md` | Config frame + **the full UUID allocation table** + the as-built `f004` notes |
| `Gap-Analysis-App-vs-Firmware.md` | app-facing view |
| `docs/PENDING.md` | what the port deferred (`P1`–`P9`) |
| `docs/REVIEW_FINDINGS.md` | code-review defects (`R1`–`R22`), none closed |
| `docs/PORTING_FROM_STM32.md` | §2.5a — the ESP32-only modules and why they diverge |
