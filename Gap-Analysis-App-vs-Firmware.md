---
tags: [firmware, ble, app, planning, gap-analysis]
updated: 2026-09-15 (third revision)
supersedes: 2026-08-25 original, 2026-09-14 first and second revisions
---

# Gap Analysis — App vs. Firmware

What the app needs, what the board actually provides, and what has to be built to close the difference.

Originally written after the first hardware validation (2026-08-25), revised twice on 2026-09-14, and
**revised again 2026-09-15 from the firmware side**. This revision covers everything on `main` since the
second revision — `f006` Diagnostics, a **100x money error found and fixed in Live Counters**, the
definition of a "session", and `f004` Session Log (built, changed mid-session from per-pulse to
per-session rows, then **verified byte-exact on a board**) — plus two corrections to things earlier
revisions of this document got wrong.

Commits covered: `5fd868c` coin polarity default · `f107617` RTC + `AT+RTC` · `70d5403` `f002` ·
`1ce2eb3` `f003` · `b692db9` `f001` · `fe1b104` `f006` · `3fba2e9` money fix · `9776800` `f004`.

**If you read only one section, read [Needs the app team's attention](#needs-the-app-teams-attention).**

The firmware's itemized backlog is `Vendo_S1_ProductionFirmware/docs/APP_BLE_PLAN.md` (stable IDs
`D`/`H`/`M`/`B`/`X`/`A`), which is where the detail behind every claim here lives.

---

## What changed in this revision

| | |
|---|---|
| ✅ **`f004` Session Log exists and is verified on hardware** | A board returned a byte-exact 44-byte page over BLE, decoding exactly as `decodeSessionLogPage()` expects. First characteristic to be proven on its first bench run. |
| ✅ **`f006` Diagnostics built and verified** | `fe1b104`. Correct frame on a board, and it caught a real fault the first time it was pointed at one — see `F6`. |
| 🔴 **A 100x money error was found and fixed in `f003`** | `3fba2e9`. Boards flashed before that commit reported centavos where the wire expects pesos. **Any data already synced from such a board is 100x too high** — see `A11`. |
| 🔄 **"Session" now has one definition on both sides of the wire** | `3fba2e9` + `9776800`. `today_sessions` counts paid periods, and a Session Log row is one paid period. Your reference firmware counts coins for both. |
| 🔄 **A Session Log row is now one SESSION, not one coin pulse** | Product decision, 2026-09-15. Both forms were built and compared. **`denom` is 0 on every row as a result — this needs one change on your side.** |
| 🔴 **Correction: `f007` Command is NOT optional** | The second revision said it was "gated separately in the UI and can wait". Reading `BleConnectionContext.tsx` disproves that — `acknowledgeSync()` is called unconditionally with no `.catch()`. It must land with the bundle. |
| 🔴 **Correction: the coin-denomination breakdown was never reachable on an S1** | `denom` was `price_per_credit_cents / 100` — a config value, identical on every row. The mixed `P1/P5/P10/P20` spread comes from `seedDemo.ts`, not from any board. |
| ✅ **`f007` Command built — the firmware side of the bundle is complete** | All seven ops. Physical ops are gated on "is this board vending?"; `sync_ack` is recorded but never applied. |
| 🔴 **`f001` is now blocked ONLY by `A7`, which is yours** | Every characteristic the gate was waiting on exists. What stops the flip is the per-install re-key. Until that lands, no board can present as `full` without breaking already-claimed machines. |
| ❌ **Still zero of 22 `REVIEW_FINDINGS` closed** | And the vend path has still never run end to end. |

**The honest summary is unchanged in shape:** the BLE surface advanced again, the board's vend
correctness did not. Four HIGH money findings still stand.

---

## Needs the app team's attention

Four items, newest first. `A8` is the only one that needs code on your side.

### 🔴 A8 — exclude `denom == 0` from the coin breakdown *(new, 2026-09-15)*

A Session Log row is now **one paid period**, carrying the total billed for it:

```
customer inserts P20 at 10:00 and P100 at 10:05 into the same run
  -> ONE row:  seq=N, ts=10:00, ts_unverified=0, denom=0, amount=120
```

A session has no coin face value, so firmware writes `denom = 0` — a value that cannot be mistaken for a
real coin, which is the point. **`AnalyticsScreen` groups rows by `denom`, counts "N coins" and draws a
coin icon, so it will render a "P0 — 1 coin" row per session until it filters those out.**

- **Revenue is unaffected.** Gross, `DailyStat` and the sync cursor key off `amount` and `seq`.
- **Nothing real is lost.** See the correction above — an S1 has one wire from the coin acceptor and sees
  only pulses. It cannot tell a P5 from a P20, and never could.
- **If 0 is not the marker you want, say so.** The field is your allocation; firmware will follow whatever
  you assign (a reserved value, a type field, anything).

### 🟡 A9 — a long-offline board loses rows, and only you can surface it *(new, 2026-09-15)*

The log is a 128 KB ring holding ~8,100 rows and wrapping oldest-first. A client further behind than that
silently receives the oldest row still retained.

**The wire has no "you missed some" field and firmware did not invent one.** It does not need one: the
backend already stores `machine.lastSeq` and receives the first row's `seq`, so a gap is exactly
`firstSeq > lastSeq + 1`. **Nothing acts on that today.** Firmware exposes the oldest retained seq over
the serial console for bench work. If you want it flagged in the UI or the sync response, it is your call
whether to derive it or ask for a field.

### 🟡 A10 — bench data will arrive with `ts_unverified = 1` *(new, 2026-09-15)*

A board whose clock has never been set writes rows with `ts = 0, ts_unverified = 1`. Your handling is
already correct — the backend files them and excludes them from `DailyStat` — but expect to see them in
early test data, and note the timestamp can never be repaired afterwards. Firmware sets the clock from
`f002` Time Sync or `AT+RTC=`.

### 🔴 A11 — anything synced from a pre-`3fba2e9` board is 100x too high *(new, 2026-09-15)*

`f003` Live Counters was sending **centavos** in `today_amount` and `lifetime_amount`, where the app
renders those figures with no division (`formatCurrency` is just `P${amount}`). A board that had taken
P50 reported `5000`.

Fixed in `3fba2e9`: the conversion now happens once, at the wire boundary. **If any board was ever synced
to the backend before that commit, its `Machine`, `DailyStat` and `Heartbeat` rows are inflated 100x and
need checking or purging.** We believe no such sync happened outside the bench, but you are the only side
that can confirm it against real data.

**The units convention as it now stands, which is worth you confirming rather than inferring:**

| Characteristic | Unit |
|---|---|
| `f003` Live Counters — `today_amount`, `lifetime_amount` | **PESOS** |
| `f004` Session Log — `amount`, `denom` | **PESOS** |
| `f005` Config — `price_per_credit_cents`, `coins_required` | **CENTAVOS / whole pesos** |

That inconsistency is in the contract, not in our implementation — firmware follows it exactly. It is
exact rather than lossy because price-per-pulse is always a whole number of pesos. Flagging it because a
future characteristic will hit the same fork and someone should decide it deliberately.

### 🟢 A12 — `today_sessions` counts VENDS, not coins *(new, 2026-09-15)*

Your reference firmware increments `todaySessions` inside `addEvent()` — once per simulated coin. On a
real S1 that would put "Sessions today: 10" on an operator's screen for one customer who inserted a P10
coin at P1/pulse.

Firmware counts a **paid period** instead: money in, relay runs, period ends. As of `9776800` a Session
Log row means the same thing, so **`today_sessions` and the number of rows uploaded now agree** — they
deliberately did not before. No action needed unless a screen somewhere assumes the reference behaviour.

### 🔴 A7 — a per-install re-key, and it still blocks `f001` *(carried forward)*

Unchanged from the second revision, and still the most consequential app-side item. Detail below under
"Asks of the app".

---

## The one-sentence state

**The Vendo S1 now talks to the app's protocol properly, and still has not been proven to vend.** Six of
the fourteen characteristics are implemented, four of them verified on hardware, and the clock works;
meanwhile every bench-verification item in `PENDING.md` is open, all 22 `REVIEW_FINDINGS` are unresolved,
and the ported vend logic — operation modes, per-credit gating, accumulation, session expiry — has never
executed on a board.

**Every characteristic the sync sequence needs now exists.** What stands between here and a working
end-to-end sync is `A7` — an app-side re-claim path — and a board that has been proven to vend.

So the BLE column is now the *most* finished part of this product, which is exactly the misreading to
guard against. See "The rest of the S1's work in progress" before treating the table below as a
completion estimate.

## Where the line actually falls — BLE only *(revised)*

| Capability | UUID | App | Contract | Bring-up FW | **Vendo S1 today** |
|---|---|---|---|---|---|
| Config read/write | `f005` | ✅ | ✅ | ✅ | ✅ **works, hardware-verified** |
| Device identity (advertised) | — | ✅ | ✅ | ✅ | ✅ **serial is the BLE device name** |
| Device Info | `f001` | ✅ | ✅ | ✅ | ✅ **built + verified, but flag-gated** ⚠️ |
| Time sync | `f002` | ✅ | ✅ | ✅ | ✅ **built**, not yet exercised over BLE |
| Live counters | `f003` | ✅ | ✅ | ✅ | ✅ **built + hardware-verified** |
| Earnings / session log | `f004` | ✅ | ✅ | ✅ | ✅ **built 2026-09-15** — one row per session, `denom = 0`; never run on hardware |
| Diagnostics / fault codes | `f006` | ✅ | ✅ | ✅ | ✅ **built + hardware-verified** |
| Remote commands | `f007` | ✅ | ✅ | ✅ | ✅ **built 2026-09-15** — all 7 ops; never run on hardware |
| OTA firmware update | `f008`–`f00a`, `f00d` | ✅ | ✅ | ✅ | ❌ none |
| WiFi provisioning + cloud | `f00b`–`f00c` | ✅ | ✅ | ✅ | ❌ none |
| BLE bonding / encryption | — | ❌ | ✅ | ❌ | ❌ |

Flash after all of it: **651,433 bytes, 49.7%** of the 1.31 MB app partition. Headroom was never the
constraint and still isn't.

### Where the "Bring-up FW" column actually points

Worth recording, because the firmware side assumed wrongly for a while: this column is **not**
`Vendo_S1_TestCode`. That harness has no BLE at all — it is a serial hardware-test rig. The reference
implementation of all thirteen characteristics is **`firmware/esp32-bringup/src/main.cpp` inside the app
repo**. The column was correct; it was pointing at a tree the firmware side could not see.

---

# ⚠️ The constraint that governs everything now

**`f001` Device Info is a capability probe, not just a data source.**

`BleService.resolveCapabilities()` decides a board's entire profile on one test:

```ts
const profile = uuids.includes(CHARACTERISTICS.deviceInfo) ? 'full' : 'configOnly';
```

Today an S1 is `configOnly`. The app skips its whole sync sequence and goes straight to Config — which is
**why Config works on a real board**.

The moment `f001` is exposed, that same board reads as `full`, and `BleConnectionContext` runs:

`writeTimeSync` → `readDeviceInfo` → schema check → `readLiveCounters` → `readConfig` →
`pullSessionLogDelta` → `readDiagnostics` → `readWifiStatus`

The first missing characteristic throws and **the connect fails entirely — including the Config push that
works today**. Adding Device Info on its own is not an increment, it is a regression.

So `f001`, `f002`, `f003`, `f004`, `f006` **and `f007`** must land in one release, or none of them — an
earlier reading of this had Command "gated separately in the UI", which `BleConnectionContext.tsx`
disproves: `acknowledgeSync()` is called unconditionally and without a `.catch()`, so the connect throws
*after* the backend upload has succeeded. (`readWifiStatus` is caught by the app, so the WiFi set really
is not a blocker.)

**As of 2026-09-15 all six exist in firmware.** The flag is still off, for one reason that is not ours:
`A7`.

**Firmware has made this structural rather than a note to remember.** `f001` is behind its own PlatformIO
environment:

- `pio run -e esp32dev` — the default. No `f001`. Safe for any board the app will touch.
- `pio run -e esp32dev-devinfo` — bench only, exposes `f001` for nRF Connect, which has no notion of app
  profiles. Prints a warning at boot.

**The firmware side of that gate is now clear.** The flag moves into the default environment and this
variant is deleted the moment the app has `A7`'s re-claim path — that is the only thing left holding it,
and it is app-side.

---

# Asks of the app

### ✅ N3 — UUID allocation table — **CLOSED**

Resolved by reading `mobile/src/ble/constants.ts` directly. The full table is recorded in
`Vendo_S1_ProductionFirmware/docs/BLE_CONFIG_CONTRACT.md`. The earlier inference was right: `f005` for
Config really did imply `f001`–`f004` were already allocated.

Also settled: **two service UUIDs exist and both are final.** `6a40f000` is the contract's, implemented by
the app-repo bring-up firmware; `6a400001` is the S1's, which began as a placeholder and the app has since
ratified (`KNOWN_SERVICE_UUIDS` scans for both). The S1 will not switch — the app distinguishes the two
firmwares by GATT tree, not service UUID, so switching has no upside and would invalidate every flashed
board.

### 🔴 A7 — a per-install re-key, and it blocks `f001`

**New, and the most consequential app-side item.** Distinct from `A1`, which stays closed.

`A1` was closed on "no backend fleet was ever claimed under `VendoS1`". True — but the app also asserts,
on every `full` connect:

```ts
if (deviceInfo.deviceId !== machine.deviceId)
    throw new Error('Connected to the wrong board — device ID did not match.');
```

A machine claimed today stores the **BLE peripheral id** (the MAC, on Android) in `machine.deviceId`,
because there was no Device Info to read a real one from. The first time a board reports
`VLABS-S1-00001`, every already-claimed machine **in that app install** fails with *"Connected to the
wrong board."*

That guard is correct and worth keeping — it stops a connection landing on a neighbouring unit and
writing one machine's pricing into another's record. It just needs a re-claim or migration path before
`f001` ships. Cheap now; ugly once installs exist in the field.

### 🟡 A6 — two one-line fixes found in the app source

- `findBoardByDeviceId()` still requires `name === 'VENDOS1'`. This is the **reconnect fallback only** —
  the primary connect-by-peripheral-id path is fine, and discovery is by service UUID — so the symptom is
  *"Board not found nearby"* instead of a successful retry. Low severity, real.
- Their note that "every S1 on a site looks identical over the air" is now **stale in your favour**:
  serial-as-name fixes exactly that complaint.

### 🟢 N1 / N2 — carried forward from the last revision

Discover by service UUID `6a400001-0000-1000-8000-00805f9b0001`, not by device name. Key
`Machine.deviceId` on the serial, which is readable from the advertised name at scan time with no
connection. Unprovisioned boards advertise `VLABS-UNSET-<MAC last 2 bytes>` — treat that as "no identity"
and surface it rather than storing it.

---

# Firmware gaps — the BLE workstream

## F1. The event log — built, and verified on hardware 2026-09-15

**Was blocking:** all earnings, all analytics, all reporting. Now built and proven on a board — what
remains between here and a working sync is `f007` Command alone.

**Hardware evidence, 2026-09-15.** Three sessions were run on a board and read back two ways. The serial
console and the BLE characteristic returned the same three rows, and the BLE page was byte-exact against
the expected frame:

```
03 00 | 01 00 00 00 00 00 00 00 01 00 01 00 00 00
        02 00 00 00 00 00 00 00 01 00 02 00 00 00
        03 00 00 00 00 00 00 00 01 00 04 00 00 00

count=3, has_more=0, three sessions of P1 / P2 / P4
```

What that proves: the flash ring, the page builder, the GATT characteristic and the byte layout. What it
does not: pagination past the first page, persistence across a power cut, and the wrap at 8,100 rows.
The rows above carry `ts_unverified = 1` because the bench board's clock was unset — see `A10`.

**Done in this revision:**

- **The clock works.** SLM1302 driver ported from the bring-up harness with three production changes: a
  mutex (the harness was single-threaded), a coherent-read retry against mid-read rollover, and
  TZ-independent epoch conversion. The RTC seeds the system clock at boot and is written back hourly;
  `time()` is the running clock, because the ESP32's 40 MHz crystal beats an untrimmed 32 kHz watch
  crystal while powered.
- **BT1 is a CR2032 — confirmed, not assumed.** Trickle charging is written off every boot *and read back
  to confirm*, because charging a non-rechargeable cell can vent it. Enabling it now requires two
  deliberate macros and trips a compile error otherwise.
- **`f003` Live Counters is live**, including the Manila business-day boundary (below).
- **`f002` Time Sync accepts `epoch_utc:u32`**, clamped to 2026–2050 so an uninitialised client cannot
  set 1970 and corrupt log ordering permanently.

**`f004` Session Log is built (2026-09-15), and three things about it need the app team's eye:**

- **One row is one SESSION — one paid period — and `denom` is 0.** A customer inserting P20 at 10:00 and
  P100 at 10:05 into the same run produces ONE row: `ts = 10:00, amount = 120, denom = 0`. Row count and
  Live Counters' `today_sessions` now mean the same thing.

  **The app change this needs is one line: exclude `denom == 0` from the coin-denomination breakdown in
  `AnalyticsScreen`** (or hide that panel for S1 boards). Otherwise it renders a "P0 — 1 coin" row per
  session. Revenue, `DailyStat` and the sync cursor are unaffected — they key off `amount` and `seq`.

  **Worth knowing before you object:** `denom` was never reachable on this hardware. The S1 has one wire
  from the coin acceptor and sees only pulses — it cannot tell a P5 from a P20. The previous firmware put
  `price_per_credit_cents / 100` in that field, a *config* value identical on every row, so the mixed
  breakdown (`P10 | 43 coins`, `P5 | 12 coins`) only ever came from `seedDemo.ts`. Nothing real was lost.

  If 0 is not the marker you want, tell us what is — the field is your allocation and firmware will follow.

- **Money is logged when it is BILLED, not when it enters the box.** Coins inserted but not yet worth a
  credit sit banked and are carried by whichever session spends them. If the board resets first, that
  money is in the cash box and in no row. Bounded by one credit's worth of change; tied to `R4`, open on
  the firmware side.
- **Pages carry 10 events (142 bytes)**, not the 1 the bring-up firmware used. Your
  `MAX_SESSION_LOG_PAGES` of 500 then means 5,000 events per connect, against a board that retains
  ~8,100 — so a machine offline for over a week drains in two connects rather than one.
- **There is no "you missed rows" field, and we did not invent one.** The log is a 128 KB ring that wraps
  oldest-first; a client further behind than the ring gets the oldest retained event. That gap is already
  derivable server-side — `firstSeq > lastSeq + 1` against `machine.lastSeq` — but **nothing acts on it
  today**. If you want it surfaced, say how and we will add it rather than guess.

Everything the earlier version of this section asked for is in place: `{seq, ts, ts_unverified, denom,
amount}` at 14 bytes, an exclusive `after_seq` cursor on every page, `seq` monotonic across reboot (its
floor lives in the `vendocnt` NVS namespace, so neither a factory reset nor a config-magic bump rewinds
it), and `ts_unverified` set whenever the clock was never set.

- `last_seq` in Live Counters now tracks the event log, so the app's "no events" fallback in
  `acknowledgeSync()` reports a real number rather than 0.

### The business day is Asia/Manila, and the app decided that, not firmware

Worth stating because it was nearly got wrong. The app displays `todayAmount` **instead of** the backend's
own `DailyStat` gross whenever a board is connected — Dashboard, Analytics, MachineDetail and the machine
list all do `hasLiveSnapshot ? liveCounters.todayAmount : ...`.

So the board's notion of "today" has to match the backend's exactly, or the figure visibly jumps the
moment an operator connects and the two are never reconcilable. `mobile/src/utils/businessDay.ts` fixes it
at **Asia/Manila, a hard +08:00**, deliberately not the phone's timezone. Firmware now rolls `today_*` at
**16:00 UTC** to match. Verified: rollover lands exactly on the boundary and holds across UTC midnight.

### And `lifetime_amount` moves per coin, not per session

Also derived from the app source rather than guessed. `DiagnosticsScreen` proves a coin was seen by
snapshotting the counters and watching `lifetimeAmount` change. A per-session increment would report a
dead coin path on a perfectly working board — and that wizard is the sanctioned way to confirm coin
polarity without a serial console (`A3`).

## F2. Coin polarity — the default was wrong, and is now reasoned rather than guessed

**Substantially changed, and partially validated on hardware.**

The previous default (`false` = idle HIGH, pulse LOW) was a guess and has been **flipped to `true` (idle
LOW, pulse HIGH)**, derived from the S1 schematic's COIN_SLOT sheet rather than assumed:

| | |
|---|---|
| **Idle** | `+5V → R22 → R24 → U4 LED → GND` drives ~1.9 mA. LED on, phototransistor conducts, COIN_IN pulled down → **idle LOW** |
| **Pulse** | The acceptor sinks J10-2 to ~0.2 V, the LED loses forward voltage and turns off, R25 pulls COIN_IN to +3.3 V → **pulse HIGH** |

Corroborated by the fail-safe direction: an unplugged acceptor floats to +5V through R22, LED stays on,
COIN_IN reads LOW — i.e. *idle*. Under the old default a disconnected harness would have read as a
permanently asserted coin line.

**And coins now count on a real board.** `lifetime_amount` moved when a coin was dropped — the first time
this path has demonstrably worked. Not conclusive (one denomination, one board, no DMM reading), so the
item stays open, but the direction is confirmed.

### 🔴 A new hardware concern this uncovered — R25

**`R25` 1K should be 4.7K, and it is worth fixing before the schematic freezes.** 1K asks U4's
phototransistor to sink 3.3 mA against a 1.9 mA LED drive — a current transfer ratio of ~175%, and the
CTR bin is not pinned in the BOM. A low-CTR part would park COIN_IN between V<sub>IL</sub> and
V<sub>IH</sub>: not a clean logic level, and the failure is intermittent miscounting rather than an
obvious dead line. **This is money.**

Two further hardware checks came out of the same review:

- **Inhibit direction** — `COIN_EN` HIGH pulls the acceptor's inhibit pin to GND via Q3. Enable or
  inhibit? Inverted, the machine takes nothing at all.
- **Q3's symbol is `Q_NMOS_Depletion_GSD`** (C75882, Q1–Q4). If the ordered part is genuinely depletion
  mode, every gate is ON at reset and the premise behind `out_safe_low()` inverts — which would mean the
  relay and the coin enable are live during boot. A 30-second BOM check, and worth doing.

## F3. Banked money is RAM-only (`R4`)

**Unchanged. Still open.** Any reset zeroes the customer's balance, and on this board resets are routine.

Two things sharpen it since the last revision. The counters module now records money **at coin
acceptance**, which is correct for cash reconciliation — the coin is in the box the moment it is accepted.
So if a customer inserts ₱10 and the board resets before vending, `lifetime_amount` correctly records the
₱10 as taken while the customer's credit is silently gone. The counters stay right and the customer is
still out of pocket.

And it becomes visible rather than merely true once `f004` lands, because the board will then also be
*reporting* those vends.

## F4. Watchdog supervision (`PENDING.md` 1–2, `R5`–`R8`)

**Unchanged. Still open.** The TPL5010 is petted unconditionally — catches total system death, not an
application-level hang. `R6` notes this can be closed today with the ESP32's own task watchdog without
waiting to measure the TPL5010 window, and `R7` warns the enforcement as drafted would reboot healthy
boards.

Relevant to the app: diagnostics code `0x05` (watchdog reset since last sync) is how an operator would
learn this is happening. **That now exists** — `f006` shipped in `fe1b104` — so the code will appear in
heartbeats as soon as a board reboots unexpectedly. Note it cannot distinguish a watchdog reset from
someone pressing the RESET button: both drive the same `ESP_EN` net on this hardware.

## F5. BLE bonding — still nobody's

**Unchanged.** The contract specifies bonded, encrypted GATT (Just Works); neither side implements it. Any
phone in range can read and write a board's config, including pricing and coin polarity.

Two new intersections since the last revision. Board identity is now CLI-writable, and the plan considers
whether provisioning should ever move to BLE — **it must not, before bonding**, since a `set_serial` over
an unbonded characteristic needs no physical access at all. And `f002` Time Sync is now writable by any
phone in range; the 2026–2050 clamp limits the damage to a wrong-but-plausible timestamp rather than
corrupted log ordering.

## F6. Diagnostics — done; and the rest of the tree

**`f006` Diagnostics is built and verified on hardware** (`fe1b104`). Frame is
`uptime:u32, boot_ts:u32, boot_ts_unverified:u8, sensors_bitmap:u8, errors_count:u8`, then one byte per
fault code. Codes implemented:

| Code | Meaning | Notes for the app |
|---|---|---|
| `0x01` | coin pulse line fault | line held asserted for 2 s — usually wrong polarity, not a broken acceptor |
| `0x02` | relay drive fault | **never raised** — this board has no relay sense. Present for contract parity only |
| `0x03` | RTC unset / backup failure | re-raises immediately after `clear_errors` if the clock is still unset; it is a present condition, not a past event |
| `0x04` | persistent write failure | NVS or the log partition rejected a write |
| `0x05` | watchdog reset since last sync | indistinguishable from a RESET button press on this hardware |

**It earned its place the first time it was used.** A board reported `0x01` while the coin slot appeared
to work perfectly. The stored config had `coin_active_high = false` on hardware that idles LOW, so the
input read as permanently asserted: one phantom credit at power-on, then counting on the trailing edge.
Nothing visible from outside — the machine took coins and ran.

**The general hazard that exposed, which matters to anyone deploying boards:** a default fixed in firmware
(`5fd868c` corrected this very field) **only reaches boards with no stored config**. An
already-provisioned board keeps the old value forever. Config pushed over `f005` is the only way to
correct one in the field, which is an argument for the app pushing a known-good config on first connect
rather than assuming the board's defaults are current.

**`f007` Command is built (2026-09-15), and it was NOT optional** — correcting this section's previous
claim that it "is gated separately in the app's UI and can follow". `acknowledgeSync()` is called
unconditionally with no `.catch()`, so without it the connect throws after the backend upload has already
succeeded.

All seven ops are implemented. Three behaviours to expect:

- **Physical ops are refused while a session is running** (`identify`, `testRelay`, `testBuzzer`,
  `testLed`) and **dropped rather than queued** — a beep firing minutes later, after the technician has
  walked away, is worse than none. `syncAck`, `clearErrors` and `wifiForget` run at any time, so a
  mid-vend connect never fails on this account.
- **A refusal is invisible to you.** The characteristic is write-only and the contract gives it no status
  channel, so the ATT write succeeds either way. If the Diagnostics wizard needs to distinguish "tested"
  from "refused", that needs a field you would have to allocate.
- **`syncAck` is recorded, never applied.** Its param is the backend's last seq, which can legitimately be
  lower than the board's; writing it back would rewind the sequence numbers delta sync depends on.

**`wifiForget` is accepted and ignored** — there is no WiFi on this board yet.

**The `reboot` op that would close `A4` has no allocated op code** (`0x01`–`0x07` are all taken) and
firmware will not invent `0x08`. **That allocation is yours to assign.**

OTA and WiFi are unchanged: two app slots are believed present but worth verifying against a real build,
and WiFi + HTTP + JSON is what pushed the app-repo bring-up firmware to 94% of flash.

---

# The rest of the S1's work in progress

**This is what else is open on the board.** Not app concerns — but several determine whether the numbers
the app displays are worth reading.

**Zero of the 22 `REVIEW_FINDINGS` are closed.** Four HIGH ones touch money directly:

| | |
|---|---|
| R1 | `price_per_credit_cents` can be set to 0 — banks ₱0.00 per coin. **The app already clamps to a floor of 1 client-side and cites this finding by name**, but that is not a substitute for firmware rejecting it. |
| R2 | Sensor Stop has no arming requirement — a stuck sensor ends every session in ~60 ms. The app exposes it as a plain toggle. |
| R3 | `blocks * relay_on_ms` has no clamp — `uint32_t` overflow can *shorten* a paid session. |
| R4 | Banked money is RAM-only (F3 above). |

**The vend path has still never run.** What has now run on a board: BLE Config read/write, `AT+SERIAL`
provisioning and readback, `f001` Device Info, `f003` Live Counters, and coin counting. The vend state
machine itself — every operation mode, the per-credit gate, accumulation, session expiry — remains a
faithful port that compiles and has never executed.

**Bench items still open:** coin polarity confirmation with a DMM (F2 — note **step 1 is a meter reading,
not `AT+COIN?`**), the R25/inhibit/Q3 hardware checks above, the TPL5010 window, `digitalRead(SDA)`
behaviour, and the Sensor Stop rail on J6 (IO32 is not 5V-tolerant).

**Added by the identity and clock work:** the write-once serial rejection path, `AT+SERIAL_ERASE`, whether
NVS survives a reflash, and whether the RTC keeps time across a full power cut. The NVS one underpins the
entire factory provisioning procedure — `pio run -t upload` preserves NVS, **`esptool erase_flash` does
not**, and the failure mode is a board that ships with no identity and looks perfectly healthy.

**The `ENABLE_CLI` question is now harder, not easier.** `R22` flags it as a free-vend backdoor, and the
app having taken over beeps/display/polarity removed the old objection. But the serial setter is *also* a
CLI command, so dropping the CLI drops the provisioning path with it. Either manufacture flashes a
provisioning build then a production one (preserving NVS), or `ENABLE_CLI` stays and `R22` is accepted, or
provisioning moves to BLE — which requires bonding first. Needs deciding before the first production run.

---

# App gaps

## A1. ~~An S1 has no stable identity~~ — **closed for the backend, reopened as A7 for installs**

The backend-fleet half stays closed: no fleet was claimed under `VendoS1`, and Android's peripheral id is
the MAC. But per-install `machine.deviceId` records are a separate problem — see **A7** above, which is
now blocking `f001`.

## A2. Empty states for `configOnly` machines

**Unchanged, still shippable today with no firmware dependency.** Dashboard, Analytics and Reports show a
permanent ₱0 for a `configOnly` S1 with no explanation, indistinguishable from a machine that earned
nothing.

**Second state worth surfacing:** a board advertising `VLABS-UNSET-xxxx` has no identity at all. Different
problem, different message.

## A3. Coin polarity from the app — now unblocked on the firmware side

`f003` exists, so the Diagnostics wizard's coin-path step will work the moment the board presents as
`full`. Until then the bench equivalent is `AT+COUNTERS?` over serial: note `lifetime`, drop a coin, run
it again.

## A4. No way to reboot a board from the app

**Unchanged.** Most config fields need a restart and the app says so, but triggering one still needs
serial or physical access. The `reboot` op on `f007` closes it.

---

# Suggested sequence *(revised)*

0. **The board's own money bugs** (R1–R4) and **the hardware checks** (R25, inhibit direction, Q3). None
   of the below produces a number worth reading if billing is wrong underneath it, and R25 is a
   before-the-schematic-freezes item.
1. **Confirm coin polarity with a meter** (F2). The schematic-derived default is probably right and a
   coin now counts, but one DMM reading closes it properly.
2. **`A7` — the app's re-claim path.** App-side, and it blocks `f001` from ever shipping. Cheap now.
3. ~~**`f004` Session Log**~~ — built and **hardware-verified** 2026-09-15.
4. ~~**`f007` Command**~~ — built 2026-09-15, untested on hardware.
   **The ball is now app-side: `A7`** (re-claim path, blocks `f001` and therefore the whole sync) and
   **`A8`** (filter `denom == 0`, one line, makes Analytics correct the day the first real sync lands).
5. **Flip `f001` on** — move `ENABLE_BLE_DEVICE_INFO` into the default environment. Only now is it safe,
   and only if step 2 landed.
6. **Prove the vend path on hardware** (F5 of the board's own list). Independent of all the above and
   arguably should outrank it.
7. **Decide banked-money persistence** (F3). A decision, not necessarily code.
8. **Watchdog supervision** (F4) via the task-watchdog route.
9. **Bonding** (F5), before WiFi ships credentials and before any thought of BLE provisioning.
10. **`f007` Command, then OTA and WiFi** (F6).

## Related
- `Vendo_S1_ProductionFirmware/docs/APP_BLE_PLAN.md` — the itemized backlog behind this document
- `Vendo_S1_ProductionFirmware/docs/BLE_CONFIG_CONTRACT.md` — the 37-byte Config frame **and the full
  UUID allocation table**
- `Vendo_S1_ProductionFirmware/docs/HANDOFF.md` — session context for picking this up fresh
- [[Board-Firmware-Contract]] — the target contract
- `Vendo_S1_ProductionFirmware/docs/PENDING.md` — firmware's own deferred list (`P1`–`P9`)
- `Vendo_S1_ProductionFirmware/docs/REVIEW_FINDINGS.md` — code-review defects (`R1`–`R22`), none closed
