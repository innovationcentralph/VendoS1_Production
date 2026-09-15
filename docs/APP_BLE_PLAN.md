# App/BLE Integration Plan — itemized backlog

Derived from `Gap-Analysis-App-vs-Firmware.md` (app team, 2026-08-25), cross-checked
against this repo's `docs/PENDING.md`, `docs/REVIEW_FINDINGS.md`,
`docs/BLE_CONFIG_CONTRACT.md` and the shipping `src/ble_config.cpp` on 2026-09-09.

**Stable IDs, same convention as `REVIEW_FINDINGS.md`.** Six prefixes:

| Prefix | Meaning |
|---|---|
| `D`  | **Decision** — needs a person, not code. Blocks the item that depends on it. |
| `H`  | **Hardware/bench** — only a board can close it. No code. |
| `M`  | **Money correctness** — gates whether any number BLE reports is worth reading. |
| `B`  | **BLE build-out** — a new characteristic or GATT behaviour. |
| `X`  | **Cross-cutting** — engineering work several `B` items depend on. |
| `A`  | **App/backend side** — not this repo, tracked so the handoff is explicit. |

Cross-references: `F1`–`F6` and `A1`–`A4` are the gap analysis's own IDs;
`R1`–`R22` are `REVIEW_FINDINGS.md`; `P1`–`P9` are `PENDING.md` items.

## Decision log

| When | ID | Decision |
|---|---|---|
| 2026-09-09 | `D1` | **Keep the existing service UUID** `6a400001-0000-1000-8000-00805f9b0001`. It is the contract now, not a placeholder. |
| 2026-09-09 | `D2` | **`device_id` = a serial written to NVS at manufacture, CLI-configurable.** Not MAC-derived. |
| 2026-09-09 | `D8` | Serial is **`VLABS-S1-NNNNN`** (14 chars), **write-once**, with a MAC-token-gated erase for RMA/refurb. |
| 2026-09-09 | `X6` | **Implemented** — `src/identity.h/.cpp`, NVS namespace `vendoid`, plus `AT+SERIAL` / `AT+SERIAL_ERASE` and a boot-banner line. |
| 2026-09-13 | — | Advertised name **is** the serial. `VendoS1` was only a development placeholder; the app adapts. |
| 2026-09-14 | `D10` | **Scope: Android only.** iOS is out of scope for this plan until someone says otherwise. |
| 2026-09-14 | `D11` | **No fleet was ever claimed under `VendoS1`**, so there is no backend migration to perform. `A1` is closed. |
| 2026-09-14 | `D12` | **Time is UTC**, `epoch_utc:u32`, clamped 2026–2050. Settled by the app contract, not chosen here. |
| 2026-09-14 | `D13` | **BT1 is a CR2032.** Trickle charging written off every boot and read back. Enabling needs two macros. |
| 2026-09-14 | `D14` | **Business day is Asia/Manila (+08:00)**, rolling at 16:00 UTC — matching the app's `businessDay.ts` and the backend's `DailyStat`. |
| 2026-09-14 | — | **`f001` ships OFF by default** in its own PlatformIO environment. It is a capability probe; shipping it early is a regression, not an increment. |
| 2026-09-14 | `D15` | **BLE money is PESOS, not centavos.** The app renders these figures with no /100 and its reference firmware accumulates whole pesos. Conversion happens only in `counters_serialize()`. Exact — price-per-pulse is always whole pesos. |
| 2026-09-14 | `D16` | **A session is one paid period** (money in → relay runs → period ends), counted at `APP_STATE_SESSION_END`. Not pulses, not relay edges, not credits. Deliberately diverges from the app's reference firmware — see `src/counters.h`. |
| 2026-09-15 | `D7` | **Event log is a 128 KB dedicated flash partition (`vendolog`), oldest-first wrap, 10-event pages.** ~8,100 events, ~8 days at P1,000/day. NVS was rejected: 20 KB total, and it holds the write-once serial. Nothing below 0x290000 moves, so identity survives the repartition. |
| 2026-09-15 | `D17` | **No "you missed rows" field was invented.** The backend already stores `machine.lastSeq` and receives the first event's seq, so a wrap gap is `firstSeq > lastSeq + 1` server-side. Firmware exposes the oldest retained seq via `AT+LOG?`. |
| 2026-09-15 | `D18` | **A Session Log row is one SESSION, not one coin pulse** (user's decision, overriding the per-pulse build earlier the same day). One paid period = one row carrying the total billed, stamped at its first billing. `denom` is therefore **0** on every row — a session has no coin face value. **Needs the app team's sign-off**, see below. |
| 2026-09-15 | `D19` | **`denom` on this hardware was never a coin denomination anyway.** It is `price_per_credit_cents / 100` — a config value, identical on every row, because the board sees pulses on one wire and cannot tell a P5 from a P20. The app's mixed-denomination breakdown can only ever have come from its simulator. |

`D10` and `D11` together collapse a large part of this plan — see `A1` and `B1`.
The short version: the whole "board identity is a data-integrity emergency"
thread came from iOS peripheral ids being unstable and from a fleet accumulating
MAC-keyed rows. Neither applies. On Android the OS hands the app the MAC as the
peripheral id and the advertisement now hands it the serial, so identity is
solved at scan time with no characteristic involved.

---

## Status board

| ID | Pri | Kind | Item | Blocks | Done |
|---|---|---|---|---|---|
| D1 | — | decision | ~~Service UUID is a placeholder~~ — **decided: keep as-is, it is now the contract** | — | `[x]` |
| D2 | — | decision | ~~What is a board's `device_id`?~~ — **decided: factory serial in NVS, CLI-writable** | — | `[x]` |
| D3 | P1 | decision | Banked money: persist across reset, or accept the loss? (`R4`/`F3`) | M4 | `[ ]` |
| D4 | P1 | decision | Is `price_per_credit_cents == 0` legitimate? (`R1`) | M1 | `[ ]` |
| D5 | P2 | decision | Bonding + whether `ENABLE_CLI` ships on field units (`R22`/`F5`) | B9, X7 | `[ ]` |
| D6 | P2 | decision | Live-apply vs reboot-to-apply for the other 19 config fields | X2, B6 | `[ ]` |
| D7 | — | decision | ~~Event-log retention~~ — **decided 2026-09-15: 128 KB `vendolog` partition, 8,192 slots, oldest-first wrap** | — | `[x]` |
| D8 | — | decision | ~~Serial format + write policy~~ — **decided: `VLABS-S1-NNNNN`, write-once, token-gated erase** | — | `[x]` |
| D9 | P1 | decision | **Digit ceiling:** 5 digits caps the fleet at 99,999. Free to widen now, messy after the first production run | factory run | `[ ]` |
| H1 | **P0** | bench | Coin polarity — **derived from schematic 2026-09-14 and default flipped to `true`**; needs one measurement to close, and **step 1 is a DMM reading, not `AT+COIN?`** (`P3`/`F2`) | everything | `[ ]` |
| H5 | **P1** | hw | **R25 1K → 4.7K before the schematic freezes.** 1K asks U4 to sink 3.3 mA off a 1.9 mA LED (CTR ≥ ~175 %, bin unpinned in the BOM); a low-CTR part parks COIN_IN between VIL and VIH | H1 | `[ ]` |
| H6 | P1 | bench | Inhibit direction: `COIN_EN` HIGH pulls the acceptor inhibit pin to GND via Q3 — enable or inhibit? Inverted, the machine takes nothing. Same sitting as `H1` | vend path | `[ ]` |
| H7 | P2 | hw | Q3 symbol is `Q_NMOS_Depletion_GSD` (C75882, Q1–Q4). If the ordered part really is depletion, every gate is ON at reset and `out_safe_low()`'s premise inverts. 30-second BOM check | strapping safety | `[ ]` |
| H2 | P1 | bench | Prove the vend path on hardware at all — the port is unrun | everything | `[ ]` |
| H3 | P2 | bench | Measure the TPL5010 window (`P2`/`R8`) | M5 tuning | `[ ]` |
| H4 | P3 | bench | `digitalRead(SDA)` behaviour (`P4`); Sensor Stop rail on JP9/IO32 (`P5`) | M2 | `[ ]` |
| M1 | P1 | money | Floor on `price_per_credit_cents` (`R1`) | trustworthy earnings | `[ ]` |
| M2 | P1 | money | Sensor Stop arming requirement (`R2`) | trustworthy sessions | `[ ]` |
| M3 | P1 | money | Clamp `blocks * relay_on_ms` overflow (`R3`) | trustworthy sessions | `[ ]` |
| M4 | P1 | money | Persist banked money (`R4`) — pending D3 | customer trust | `[ ]` |
| M5 | P2 | robust | Application supervision via ESP32 TWDT (`R6`, not `P1`'s draft — see `R7`) | B5 fault `0x05` | `[ ]` |
| X1 | P1 | eng | Centralize config validation in `config.cpp` (contract Q3, `R18`, `R19`) | B6, future writers | `[ ]` |
| X2 | P2 | eng | Cross-task config apply path (app task ← BLE/CLI) | B6, D6 | `[ ]` |
| X3 | P3 | eng | Flash/RAM budget gate before WiFi lands | B8 | `[ ]` |
| X4 | P3 | eng | Record BLE as intended divergence in `PORTING_FROM_STM32.md` §2/§3 | diffability rule | `[ ]` |
| X5 | P3 | eng | Stale comment: `main.cpp:118` still says Bluedroid; build is NimBLE | — | `[ ]` |
| X6 | — | eng | ~~Serial outside the `appcfg` blob~~ — **built: `src/identity.h/.cpp`, NVS ns `vendoid`, `AT+SERIAL`** | — | `[x]` |
| X7 | P1 | eng | Provisioning path if `ENABLE_CLI` is dropped (`D5`) — and note `erase_flash` wipes the serial | B1 in production | `[ ]` |
| B1 | ~P3~ | ble | **BUILT + hardware-verified 2026-09-14**, but shipped OFF: `[env:esp32dev-devinfo]` only. It is the app's capability probe — enabling it before `B3`/`B5` breaks the working Config flow | **blocked by B3, B5, A7** | `[~]` |
| B2 | — | ble | ~~RTC + Time Sync~~ — **BUILT 2026-09-14.** `src/rtc.*`, `AT+RTC`, `f002`. BT1 confirmed CR2032, trickle locked off. Power-cut retention still unverified | — | `[x]` |
| B3 | — | ble | ~~Session Log `f004` + delta sync~~ — **built + hardware-verified 2026-09-15** (byte-exact 44-byte page from a board): `src/eventlog.*` + `src/ble_sessionlog.*`, `AT+LOG?` | — | `[x]` |
| B4 | — | ble | ~~Live Counters~~ — **BUILT + hardware-verified 2026-09-14.** `f003`, Manila business day, per-coin lifetime, notify on change | — | `[x]` |
| B5 | **P1** | ble | Diagnostics `f006` — **on the critical path**: B1 cannot ship without it. Mostly plumbing, the data already exists | B1 | `[ ]` |
| B6 | P3 | ble | Command characteristic — incl. `reboot` (`F6`, closes `A4`) | A4 | `[ ]` |
| B7 | P4 | ble | OTA (`F6`) | field updates | `[ ]` |
| B8 | P4 | ble | WiFi provisioning + cloud check-in (`F6`) | cloud | `[ ]` |
| B9 | P2 | ble | Bonding / encryption (`F5`) — **before B8 carries credentials** | B8 safely | `[ ]` |
| A1 | — | app | ~~Re-key `Machine.deviceId`~~ — closed by `D11` for the *backend fleet*. **Partially reopened 2026-09-14 as `A7`** for per-install app records | — | `[x]` |
| A2 | P3 | app | Empty states for `configOnly` machines on earnings surfaces | support load | `[ ]` |
| A3 | P3 | app | Polarity confirmation in the Diagnostics wizard (needs B4) | — | `[ ]` |
| A4 | P3 | app | Reboot button (needs B6) | — | `[ ]` |
| A5 | — | app | ~~UUID allocation table~~ — **closed 2026-09-14 by reading `mobile/src/ble/constants.ts`.** Full table recorded in `BLE_CONFIG_CONTRACT.md`. Device Info is `f001`, Time Sync `f002`, Live Counters `f003`, Session Log `f004`, Diagnostics `f006`, Command `f007`, OTA/WiFi `f008`–`f00d` | — | `[x]` |
| A6 | **P1** | app | Two one-line app-side fixes found in their source: `findBoardByDeviceId()` still requires `name === 'VENDOS1'` (reconnect *fallback* only — primary connect-by-id path is fine), and a re-claim/migration path is needed before `B1` ships (see `A7`) | reconnect retry | `[ ]` |
| A7 | **P0** | app | **`machine.deviceId` re-key after all.** `A1` was closed on "no backend fleet", but the app asserts `deviceInfo.deviceId !== machine.deviceId` and an S1 claimed today stores the *BLE peripheral id* there. Every already-claimed machine breaks with "Connected to the wrong board" the moment `B1` reports a real serial | B1 | `[ ]` |
| A8 | **P1** | app | **Exclude `denom == 0` from the Analytics coin breakdown** — Session Log rows are sessions now, not coins (`D18`). One line app-side; firmware will follow a different marker if they prefer one | — | `[ ]` |
| A9 | P3 | app | Decide whether a wrap gap (`firstSeq > lastSeq + 1`) should be surfaced — derivable server-side today, nothing acts on it (`D17`) | — | `[ ]` |
| A11 | **P1** | app | **Check the backend for 100x-inflated money** from boards flashed before `3fba2e9` — Live Counters sent centavos where the wire expects pesos. Also confirm the units table in `BLE_CONFIG_CONTRACT.md` | — | `[ ]` |
| A12 | P3 | app | Note that `today_sessions` counts **paid periods**, not coins — diverges from the reference firmware (`D16`), and now matches the Session Log row count | — | `[ ]` |

---

# Decisions (D)

## D1. [DECIDED 2026-09-09] Service UUID stays as-is

**Resolution: keep `6a400001-0000-1000-8000-00805f9b0001`.** It was invented in
this repo as a placeholder, because `Board-Firmware-Contract.md` was not available
here and only the *characteristic* UUID (`6a40f005-…`) came from the contract. The
app has since been built and hardware-verified against it, so it is adopted as the
contract rather than churning a working, deployed pairing.

**Done in this revision:** the `PLACEHOLDER` / `TODO` comment block in
`src/ble_config.cpp` is replaced with a ratification note — including an explicit
"do not 'fix' this" warning, so the next reader doesn't helpfully undo the
decision — and `BLE_CONFIG_CONTRACT.md`'s ⚠️ warning is replaced with the same.

**Two things a future reader needs, both now in that code comment:**

- The tail `0000-1000-8000-00805f9b0001` resembles the **Bluetooth SIG base UUID**
  (`...00805f9b34fb`) but differs in the final group. That difference is what makes
  it safe: it cannot collide with a SIG 16-bit UUID, and no stack will try to
  shorten it to 16 bits. It only *looks* SIG-ish.
- **Config is `6a40f005`.** That numbering strongly implies the app team's
  `Board-Firmware-Contract.md` already allocates `f001`–`f004`, and probably
  beyond. So the remaining characteristics must **not** be numbered by picking the
  next free value from this side — two independently-maintained docs each choosing
  "the next one" is how a collision ships. Getting that table is `A5`, and it now
  gates `B1`.

## D2. [DECIDED 2026-09-09] `device_id` is a factory serial in NVS, CLI-configurable

**Resolution: not MAC-derived.** A serial is written to NVS at manufacture and is
settable over the CLI.

This is the more expensive of the two identity options and it buys something real:
a human-readable serial survives a board swap in the operator's mental model, and
it decouples identity from a specific ESP32 module. Four consequences follow. The
first two are P0 because they are cheap now and expensive once a fleet is
provisioned.

### 1. It must not live in the `appcfg` blob — `X6`

Putting the serial in `AppConfig` is the obvious move and it would be wrong, for
three independent reasons:

- `config_load()` falls back to `config_defaults()` on a magic mismatch or a short
  blob — so a board would **silently reassign its own identity** as a side effect
  of that guard working correctly.
- `CLAUDE.md` requires bumping `APP_CONFIG_MAGIC` on any `AppConfig` layout
  change. Every future config field would therefore erase every board's serial.
- A factory reset is meant to clear *settings*, not identity.

So: **its own NVS namespace** (proposal: `vendoid`/`serial`), read and written by
nothing in `config.cpp`. Pleasant side effect — no `AppConfig` change means no
magic bump and no change to the 37-byte Config frame, so the one
hardware-verified part of the contract is untouched by all of this.

### 2. A CLI-writable identity is a spoofable identity — `D8`

Anyone with a USB-serial cable at J7 can set board A's serial to board B's. The
two boards then report the same identity and their earnings merge into one backend
row — quietly, and in whichever direction suits whoever did it. Same class of
exposure as `R22`'s `AT+COINS_REQ` backdoor, but it corrupts data rather than
giving away a vend, so it never shows up in a till count.

Not a reason to reverse D2 — just a sub-decision D2 now requires. See `D8`.

### 3. ~~`B1` must report the MAC too~~ — superseded by `D10`/`D11`

**This argued that Device Info MUST carry the MAC, and that is no longer true.**
The reasoning was: the serial is not the MAC, so re-keying an already-claimed
board needs a correlation key, and on iOS the OS refuses to reveal a peripheral's
MAC — so the board would have to report it or every claimed board orphans.

Both legs are gone. `D10` scoped the project to **Android**, where the OS supplies
the MAC as the peripheral id. `D11` established that **no fleet was ever claimed
under `VendoS1`** — it was a development placeholder — so there is no migration.

What actually solves identity on Android is smaller than any of this: the board
advertises its serial, so a single scan yields the MAC (from the OS) and the
serial (from the advertisement) together, without connecting and without a
characteristic. Keep `mac` in `B1`'s payload — it is 6 bytes and it is the
`AT+SERIAL_ERASE` token source — but nothing depends on it.

*(Left in place rather than deleted: the iOS constraint is real and will come
back the day iOS is in scope. If that happens, re-read this section before
assuming Device Info is optional.)*

### 4. Boards already in the field have no serial

An unprovisioned board has to be representable: a `serial_valid` flag (or a
zero-length serial) in Device Info, with the app continuing to key on the MAC
until a serial appears. Define it now so the app implements it once — and note it
is also the *steady-state* behaviour for any board that reaches a customer before
the factory step exists.

**Format**, for `D8` to confirm with the app team: ASCII `[A-Z0-9-]`, up to 16
chars, carried as a fixed 20-byte NUL-padded field so the wire format stays
fixed-width like the rest of the contract. The setter validates the charset.

## D3. [P1] Banked money persistence — the product decision behind `R4`

`s_coin_value_cents` (`src/periph.cpp:212`) is RAM-only. Any reset zeroes a
customer's balance, and on this board resets are routine, not exceptional. Three
positions, pick one:

- **Persist on every change** — safest for the customer, and the write volume is
  real: NVS wear on a busy machine. Needs a wear estimate before committing.
- **Persist debounced (e.g. 2 s idle, and on session end)** — loses at most the
  last coin or two, cuts writes by orders of magnitude. Probably the answer.
- **Accept the loss, document it** — defensible only while resets are rare, which
  `H3` has not yet established.

This gets *more* urgent once `B3` lands, because then the board also reports those
vends and the numbers won't reconcile.

## D4. [P1] Is `price_per_credit_cents == 0` a legitimate value?

`R1`: zero means the machine banks ₱0.00 per coin — takes money, counts nothing.
The app can send it: **the contract's own range is `0–99,999` and the app clamps
to that range**, so this is a contract bug, not just a firmware bug. If zero is
not legitimate, the floor belongs in three places — the contract doc, the app's
validator, and `X1`'s centralized firmware validator. Free-vend, if that's a real
product mode, wants its own explicit flag rather than a price of zero.

## D5. [P2] Bonding, and whether `ENABLE_CLI` ships

Two linked calls the gap analysis correctly pairs:

- Today any phone in range can write a board's pricing and coin polarity. The
  contract specifies bonded, encrypted GATT (Just Works); neither side implements it.
- `R22` flags `ENABLE_CLI` on a field unit as a free-vend backdoor
  (`AT+COINS_REQ=1` + `AT+RELAY_MS=3600000`). The historical objection — the CLI
  is the only way to set beeps/display/polarity without reflashing — **is gone,
  because BLE now sets all of those**. So dropping `ENABLE_CLI` becomes a clean win,
  *provided* `B9` lands so BLE isn't itself the open door.

Decide both together. Note the interaction with `A1`: bonding changes what
re-pairing costs an operator.

**`D2` adds a third strand to this decision.** The serial setter is a CLI command,
so dropping the CLI also drops the provisioning path. That is `X7`, and it has to
be answered in the same breath as this one.

## D6. [P2] Live-apply vs reboot-to-apply

Today BLE matches the CLI (persist now, apply on reset) for 19 of 20 fields;
`coin_active_high` alone applies live via `coin_counter_set_polarity()`. The LCD
menu applies everything live. `BLE_CONFIG_CONTRACT.md` open question #1 recommends
BLE match the LCD; the implementation deferred that because `app.cpp` has no
cross-task path to hand a new `AppConfig` to the running app task (`X2`).

Cheap interim: a `reboot` op in `B6` closes `A4` and makes the current behaviour
acceptable. Full live-apply is `X2` and can follow.

## D7. [DECIDED 2026-09-15] Event-log retention

**A dedicated 128 KB flash partition, `vendolog`, wrapping oldest-first.**
8,192 slots of 16 bytes, so ~8,100 events retained — about 8 days on a machine
taking P1,000/day at P1 per pulse. Implemented in `src/eventlog.*`.

**Why not NVS**, which was the obvious first answer: the `nvs` partition is
`0x5000` — 20 KB, four usable 4 KB pages after NVS reserves one for compaction —
and it already carries the config blob, the **write-once factory serial**, the
earnings totals and NimBLE's bonding records. A week of retention is ~100 KB. It
does not fit, and rewriting a multi-KB blob per coin would burn the partition
that holds the board's identity.

**Where the 128 KB came from:** the stock `default.csv` allocates 1.4 MB to a
`spiffs` partition that nothing in this firmware mounts. `partitions_vendo.csv`
takes 128 KB of it and leaves **every offset below 0x290000 untouched** — `nvs`
stays at 0x9000, `app0`/`app1` keep the offsets baked into the OTA data. That is
the rule when editing that file: moving `nvs` orphans every provisioned board.

**Wrap is oldest-first** (user's decision): filling the ring erases the oldest
4 KB sector, dropping 256 events, and the surviving window stays contiguous.

**The "you missed rows" problem dissolved rather than needing a protocol
change** (`D17`). The backend stores `machine.lastSeq` and receives the first
event's `seq`, so a gap is exactly `firstSeq > lastSeq + 1` — detectable
server-side with nothing added to the wire, and nothing invented unilaterally.
`AT+LOG?` prints the oldest retained seq for the bench. **Still worth telling the
app team**, since nothing acts on it yet.

### Two sizing facts worth keeping

- **The app caps a single sync at `MAX_SESSION_LOG_PAGES` (500) pages.** At 10
  events per page that is 5,000 events per connect, less than the ring's 8,192 —
  a board that has been offline for over a week needs two connects to drain. At
  the reference firmware's 1 event per page it would have been **500 events per
  connect**, which is the strongest argument for the larger page.
- **Retention is a function of price, not of days.** At P1/pulse a P1,000 day is
  1,000 events; at P5/pulse it is 200. The 8-day figure is the pessimistic end.

## D8. [DECIDED 2026-09-09] Serial is `VLABS-S1-NNNNN`, write-once

**Format: `VLABS-S1-NNNNN`** — fixed prefix plus exactly 5 digits, 14 characters,
validated at the setter. Prefix matching is case-insensitive on input but the
canonical spelling is what gets stored, so two boards can never differ only by
case.

The prefix is abbreviated deliberately: it is identical on every board, so it
carries no discriminating information — its only job is making a serial
recognizable to a human on a sticker or in a support call, which 5 characters
does as well as spelling the brand out would. "PH" is part of the company name
rather than a region marker, so nothing in the format varies by market.

**Write policy: write-once, with a token-gated erase.** `AT+SERIAL=` succeeds only
while unset. `AT+SERIAL_ERASE=<token>` clears it for RMA/refurb, where the token
is the last 3 bytes of the board's own MAC (`AT+SERIAL_ERASE?` prints it). The
token is not a secret and isn't meant to be — it exists so that clearing an
identity requires being able to read the board in your hand, so a mistyped or
pasted command cannot orphan a different unit.

The write-once rule is enforced in `identity_serial_set()`, not in the CLI, so a
later BLE provisioning path or factory jig inherits it rather than having to
remember it.

### `B1`'s wire format — 24 bytes, and why the slack matters

**Use a 24-byte NUL-padded field in Device Info.** At 14 characters the format
leaves 9 bytes of slack, which is the point rather than waste.

The BLE contract uses fixed-width, fixed-offset frames (the 37-byte Config frame
is the precedent — `relay_on_ms` is *always* at offset 1). So a text field is a
fixed block of bytes, and its size is a layout decision, not a parser detail:

- A format change that **still fits** is a one-line edit to
  `identity_serial_format_ok()` in firmware. The app never learns about it.
- A format change that **does not fit** shifts every later field's offset and the
  frame's total length. That means a `schema_version` bump, a coordinated
  two-sided release, and — because boards already in the field keep reporting the
  old layout and there is no OTA yet (`B7`) — **an app that parses both layouts
  indefinitely.**

Nine spare bytes cost nothing: Device Info is read once per connection, so this is
not measurable in flash, RAM, MTU or airtime. An earlier draft of this plan
proposed 20 bytes, which would have fit the original 19-character format exactly
and walked into the second bullet. Flag the 24 to the app team along with `A5`.

### Still open — `D9`, the digit ceiling

**5 digits caps the fleet at 99,999 units.** Widening to 6 costs nothing today —
the field has 9 spare bytes and it is one constant in `identity.h`. Doing it
*later* is the messy case: 5- and 6-digit serials would coexist in the field and
sort and display inconsistently, and boards already provisioned cannot be
renumbered without the erase token. **Decide before the first production run.**

Also unanswered, and cheaper now than later: **a check digit.** The serial is
write-once and human-typed, so a fat-fingered `VLABS-S1-00132` for `00123`
becomes a permanent wrong identity that needs `AT+SERIAL_ERASE` to undo — and may
silently collide with a real board later. One check digit catches nearly all
single-digit errors and all adjacent transpositions, for about ten lines in
`identity_serial_format_ok()`. Worth it if a technician types the serial;
skippable if the factory step is a script that generates and sends it.

### Still open — the factory process, not the firmware

**Who runs the provisioning step, and how is it verified?** A serial a technician
forgets to set produces a board that falls back to `D2` §4's unprovisioned state
and, from the backend's point of view, never gets claimed properly. The firmware
now makes that loud in three places — the boot banner, `AT+SERIAL?`, and (once it
exists) `B5` diagnostics — but nothing *enforces* it. Worth an explicit line in
whatever the factory checklist is, alongside `X7`'s `erase_flash` warning.

---

# Bench items (H) — only a board closes these

## H1. [P0] Coin polarity — `PENDING.md` item 3, gap analysis `F2`

**Reframed 2026-09-14: this is no longer a guess, it is a prediction off the
schematic awaiting one measurement.** Full derivation in `PENDING.md` item 3.

The S1 schematic was read directly
(`../../hardware/VendoLabs_Board_S1/Vendo_main_board_ESP32`). At idle the
acceptor output does not sink, so R22/R24 drive ~1.9 mA through the U4 LED, the
phototransistor conducts, and **COIN_IN is pulled LOW**. A coin makes the
acceptor sink J10-2, the LED turns **off**, and R25 pulls **COIN_IN HIGH**. So
the correct value is `coin_active_high = true` — the **opposite** of what the
firmware shipped.

**Done in firmware 2026-09-14** (not bench-verified, analysis only):

- `kDefaults` in `src/config.cpp` flipped to `true`, with the derivation recorded
  on the field in `src/config.h`.
- `pinMode(PIN_COIN_IN, INPUT_PULLUP)` → `INPUT` in `src/periph.cpp`. R25 is a
  hard 1K external pull-up; the internal ~45K was fighting the opto, and the
  comment justifying it had the polarity backwards.

The old default was not merely wrong, it was quietly wrong: traced through the
debounce state machine it yields **one free credit on every power-on** and then
counts on the trailing edge, while otherwise appearing to work.

**The remaining risk is analog, and it is a hardware fix, not a firmware one.**
R25 = 1K asks the opto to sink 3.3 mA off a 1.9 mA LED — CTR ≥ ~175 %, on a part
whose BOM line does not pin the CTR bin. A low-CTR part never reaches a valid
logic LOW and instead parks COIN_IN between VIL and VIH, where it reads fine warm
and drifts with temperature. **Recommendation while the schematic is still open:
R25 1K → 4.7K.** See `PENDING.md` item 3.

**What still needs a board** — and step 1 needs a meter, not `AT+COIN?`:

```
1.  no coin: MEASURE COIN_IN.  expect < ~0.4 V; > ~0.5 V => raise R25
2.  drop a coin        <- count rises by exactly 1
3.  power-cycle        <- count starts at 0, not 1
4.  unplug the acceptor<- reads idle, counts nothing
5.  AT+COIN_EN=0/1     <- confirm the inhibit direction (see below)
6.  time the pulse     <- must exceed the 15 ms debounce
```

`AT+COIN_POLARITY` still flips it live if the prediction is wrong.

**Two siblings to settle in the same sitting**, both unverified in the same way:
the **inhibit direction** (`COIN_EN` HIGH pulls the acceptor's inhibit pin to GND
via Q3 — whether that enables or inhibits depends on the acceptor; inverted, the
machine takes nothing), and **Q3's depletion-mode schematic symbol**, which if it
matches the ordered part would invert the premise of `out_safe_low()` board-wide.

**Still `P0`, and still gates `B3`** — there is no point building an event log on
a counter nobody has watched count. But the *design* of `B3` is no longer blocked:
we now know which edge it will be counting.

The app can drive the flip over BLE during this session (it is the one
live-applied field), but it cannot *observe* the result until `B4` — so today
this needs a serial console alongside. That is `A3`.

## H2. [P1] Prove the vend path on hardware

The README is explicit: nothing here has run on a board beyond the BLE session.
"Faithful port that compiles" and "verified vending" are different claims and only
the first is true. Every mode (Press to Start, Pause/Resume, Auto Start), the
per-credit gate, accumulation, and session expiry want one pass each on a bench
rig. Not BLE work, but it gates whether any BLE-reported number means anything.

## H3. [P2] Measure the TPL5010 window — `PENDING.md` item 2

`R8` argues R34 = 40.2K sits well up the part's resistor/interval table, so the
real window is likely **seconds to minutes**, not ~100 ms — in which case several
constraints this codebase is built around relax considerably. Measure with the
bring-up harness (`AT+WDT=0`, then `AT+INFO?`); don't relax anything on the
inference alone.

Relevant to BLE: if the window really is ~100 ms, a `config_save()` NVS write from
a NimBLE callback deserves a hard look. If it's seconds, it's a non-issue.

## H4. [P3] Remaining bench checks

`PENDING.md` items 4 and 5 — `digitalRead(SDA)` while Wire owns the pin, and the
Sensor Stop rail (JP9 selects +3.3V/+5V; **IO32 is not 5V-tolerant**). The second
one gates `M2`.

---

# Money correctness (M) — gates the value of every BLE number

These are `REVIEW_FINDINGS.md` R1–R4 and R6. They are not BLE work and the gap
analysis is right to put them at step 0: *none of the BLE items produces a number
worth reading if billing is wrong underneath it.*

- **M1** (`R1`) — floor on `price_per_credit_cents`, per `D4`, landing in `X1`.
- **M2** (`R2`) — Sensor Stop arming requirement. A stuck sensor currently ends
  every session in ~60 ms, and the app exposes Sensor Stop as a plain toggle, so
  an operator can enable exactly this.
- **M3** (`R3`) — clamp `blocks * relay_on_ms`; `uint32_t` overflow can *shorten*
  a paid session.
- **M4** (`R4`) — persist banked money, per `D3`.
- **M5** (`R6`) — application supervision via the ESP32's own task watchdog, which
  is already compiled in (5 s, panic-on-expiry) and unused. **Take this route, not
  `PENDING.md` item 1's drafted `if`** — `R7` shows that draft reboots healthy
  boards, because `watchdog_liveness_stale_ms()` returns the worst age across all
  sources and `WDT_SRC_SESSION`/`WDT_SRC_MENU` legitimately stop checking in when
  their state exits. `R5` is the related correction: the "core-1 idle gives free
  supervision" claim in `main.cpp` and `CLAUDE.md` is false for this build.
  M5 feeds `B5`'s fault code `0x05`.

---

# BLE build-out (B)

## B1. [P0] Device Info characteristic

**UUID resolved 2026-09-14: `6a40f001-0000-1000-8000-00805f9b0001`** (`A5` closed).
`D1`, `D2`, `D8` and `X6` are all answered, so the serial, `serial_valid` and MAC
are sitting in `src/identity.h` ready to wire.

> ### ⚠️ But `B1` is no longer small, and it is no longer independently shippable.
>
> Reading the app source changed this item materially. **Device Info is the app's
> capability probe** — `BleService.resolveCapabilities()` is literally
> `uuids.includes(deviceInfo) ? 'full' : 'configOnly'`. An S1 is `configOnly` today,
> which is *why* Config works: the app skips the entire sync flow and goes straight
> to Config.
>
> Expose `f001` alone and the same board is classified `full`, so
> `BleConnectionContext` instead runs Time Sync → Device Info → Live Counters →
> Config → Session Log → Diagnostics. The first missing characteristic throws and
> the connect fails — **taking the Config push that works today down with it.**
>
> **So `B1` + `B2` + `B3` + `B4` + `B5` ship as one release, or none of them.**
> That folds `f001`/`f002`/`f003`/`f004`/`f006` into a single milestone and makes
> `B3` (event log) the critical path rather than a later phase. `B6` (Command,
> `f007`) and the OTA/WiFi set are gated separately in the app's UI and can still
> follow on their own.
>
> Full analysis, with the app source lines, in `BLE_CONFIG_CONTRACT.md`.

**Payload — SUPERSEDED 2026-09-14.** The fixed-width table previously proposed here
(24-byte NUL-padded serial, `serial_valid`, `mac`, `hw_revision`) **is not what the app
decodes.** `decodeDeviceInfo()` in `mobile/src/ble/codec.ts` expects, in order:

| Offset | Field | Type |
|---|---|---|
| 0 | `schema_version` | u8 — **must be exactly 1**; the app refuses to sync on any other value, newer *or* older |
| 1 | `modules_bitmap` | u8 — bit 0 door, 1 vibration, 2 buzzer, 3 gsm |
| 2 | `device_id` | `len:u8` + UTF-8 bytes |
| … | `model` | `len:u8` + UTF-8 bytes |
| … | `fw_version` | `len:u8` + UTF-8 bytes |

Consequences for the old design:

- **Length-prefixed, not fixed-width.** `D8`'s reasoning about 9 bytes of deliberate
  slack in a 24-byte field no longer applies — the wire format carries a length, so a
  longer serial costs exactly its own bytes. `D9`'s digit ceiling is now an identity
  question only, not a wire-format one.
- **No `serial_valid` field exists.** An unprovisioned board has to be representable
  some other way — simplest is to send the `VLABS-UNSET-…` string as `device_id`,
  which is already distinguishable by shape (`D2` §4). Needs confirming with the app
  team, since they will store whatever we send as the durable machine identity.
- **No `mac` and no `hw_revision`.** `D10` already removed the iOS argument for `mac`;
  on Android the OS supplies it as the peripheral id. `hw_revision` has nowhere to go
  — fold it into `model` or `fw_version` rather than adding a field, which would be a
  schema change the app rejects outright.
- `modules_bitmap` is new work: the app gates optional-module UI on it, and per the
  contract the board is the only authority on which blocks are fitted.

The serial and its validity flag are available from `src/identity.h` (`X6`), and
`fw_version` from `include/version.h` — but per the box above, `B1` is no longer
just wiring: it needs the four sibling characteristics alongside it.

**Companions** — the CLI and the advertised name are both done:

- ~~**Advertise a unique name.**~~ **Built 2026-09-13.** A provisioned board
  advertises **its serial verbatim** — `VLABS-S1-00001` — so name, `device_id`
  and the number on the sticker are one string and a board can be identified in
  a scanner list without connecting. Unprovisioned boards advertise
  `VLABS-UNSET-<MAC last 2 bytes>`; the `UNSET` token is deliberately *not* in
  the serial's shape, so a board that missed the factory step is obviously
  different rather than a plausible serial nobody recognizes. Two notes:
  - **This is a breaking change for name-based discovery** (user's call,
    2026-09-13: "App will adapt"). The old fixed `VendoS1` is gone, so any
    client matching on it stops finding boards. Clients should discover by
    `BLE_SERVICE_UUID` — that is what it is for, and `A5` should confirm the app
    now does.
  - **The name lives in the scan response, not the advertisement.** The 128-bit
    service UUID costs 18 of the advertisement's 31 bytes, plus 3 for flags,
    leaving room for roughly 8 characters of name. `VendoS1` (7) fit;
    `VLABS-S1-00001` (14) does not, and an over-long name is silently truncated
    or drops the advertisement entirely. So `ble_config_init()` builds both
    payloads explicitly — UUID in the advertisement, name in the scan
    response's separate 31-byte budget.
- ~~Release `B1` together with `A1`.~~ Moot — `D11` closed `A1`. `B1` can ship
  whenever `A5` provides a UUID; nothing waits on it.~~ **Wrong on both counts,
  corrected 2026-09-14.** `A5` is closed, but (a) `B1` cannot ship alone — see the
  box at the top of this section — and (b) `A1` is not fully closed: `D11` was
  right that no *backend fleet* needs migrating, but the app stores the BLE
  peripheral id in `machine.deviceId` for every S1 claimed so far and asserts it
  against Device Info on connect. That is now tracked as `A7`.

## B2. [P2] RTC port + Time Sync

`PENDING.md` item 9: the S1 has an SLM1302 (DS1302-family, 3-wire) on
IO25/IO26/IO4 that the STM32 never had, and **the bring-up harness already has a
working driver** at `../VendoLabs_TestCodes/Vendo_S1_TestCode/src/s1_rtc.cpp`. Mostly a port, not a
design.

Carry the harness's warning across: **`AT+RTC_TRICKLE` must stay off unless BT1 is
a rechargeable LIR cell** — trickle-charging a plain CR2032 damages or vents it.

Time Sync characteristic: app writes epoch seconds, board sets the RTC and records
that it has been set. That last bit is what `ts_unverified` (below) keys off.

## B3. [BUILT 2026-09-15] Event log + delta sync

`src/eventlog.*` (the store) + `src/ble_sessionlog.*` (the GATT surface) +
`AT+LOG?` / `AT+LOG=<rows>[,<after_seq>]`. **Compiles clean; nothing has been
run on hardware.**

**Verified on hardware the same day.** A board recorded three sessions and
returned them over BLE as a byte-exact 44-byte page (`count=3`, `has_more=0`,
P1 / P2 / P4), matching the serial console's `AT+LOG=` dump row for row. That
covers the flash ring, the page builder, the characteristic and the byte layout.
Not yet covered: pagination past page one, persistence across a power cut, and
the wrap. The bench rows carry `ts_unverified = 1` — the board's clock was unset,
which is `B2` still being unproven rather than a log fault.

**Changed the same day, on the user's call: a row is one SESSION, not one coin
pulse** (`D18`). The per-pulse form was built first, and the comparison is what
settled it — at the normal 1-pulse-per-peso acceptor wiring, P15 of coins is
fifteen rows of P1 carrying nothing the one session row does not. The row is
closed at `APP_STATE_SESSION_END`, the same boundary `counters_record_session()`
already uses, so Live Counters' `today_sessions` and the log's row count finally
mean the same thing.

**What it costs, recorded because it was accepted knowingly:** money is logged
when it is BILLED, not when it enters the box. Coins banked but not yet spent sit
in the cash box with no row yet — carried by whichever session eventually spends
them, *unless* a reset zeroes the balance first (`R4`, open HIGH). Per-pulse
logging did not have that hole. It is bounded by one credit's worth of change.

**Where it hooks:** `coin_consume_value_cents()` in `periph.cpp` accumulates —
one place every billing path already passes through, which is what keeps
`app.cpp` and `session.cpp` diffable against the STM32 — and `SESSION_END` closes
the row. One line added to each.

How the three non-negotiables were met:

- **`seq` is monotonic across reboots.** Recovered from the boot scan of the
  partition, then floored by `counters_last_seq()` (the `vendocnt` namespace) so
  that erasing or repartitioning the log cannot rewind it into ids the backend
  has already filed. `seq` is assigned **at the flash write, not at the coin** —
  an event that never reaches flash must not consume a number, because a hole in
  the numbering is indistinguishable from the buffer-overrun gap in `D17`.
- **`ts_unverified`** is set whenever `rtc_now()` is 0, sampled at the session's
  first billing (via `time()`, never the RTC chip — `rtc_valid()` bit-bangs the
  SLM1302 and takes its mutex, which has no business in the vend path).
- **Retention and wrap per `D7`.**

Two implementation facts that are load-bearing:

- **Nothing touches flash from the vend path.** The hazard is not the watchdog —
  petting is unconditional in its own task — it is that a 40 ms sector erase in
  the app task stalls the relay, the countdown and the buttons.
  `eventlog_record_session()` posts to a 32-deep queue; `eventlog_service()`
  drains it from `loop()`. Writes are immediate rather than debounced the way
  `counters` are: a lost counter tick is a rounding error, a lost *session* is a
  permanent hole in the backend's history.
- **`seq` → slot is arithmetic**, not a search: `(head + CAPACITY -
  (next_seq - seq)) % CAPACITY`. A page is ~10 reads of 16 bytes rather than a
  scan of 128 KB. Every record read back is checked against the seq that was
  asked for, so a broken invariant yields a **short page, never another
  customer's money**.

The design (wrap, the exclusive cursor, the app's own pagination loop) is
exercised by `tools/eventlog_ring_model.py` — a Python transliteration, because
there is no host toolchain here. It proves the *arithmetic*, not the C++.

**Still open:** `f007` Command. The app calls `acknowledgeSync()` with no
`.catch()`, so without it a connect throws *after* the backend upload has
succeeded and `sync_ack` never arrives. `f004` alone does not complete a sync.

## D18. [DECIDED 2026-09-15] A row is a session — and the ask that goes with it

**Firmware side is done.** What follows is what the app team has to decide,
written out so it can be sent as-is.

### What changed

A Session Log row (`6a40f004`) is now **one paid period**, not one coin pulse:

```
customer inserts P20 at 10:00, P100 at 10:05, same run of the machine
  -> ONE row:  seq=N, ts=10:00, ts_unverified=0, denom=0, amount=120
```

Previously that was twelve rows at the default price, or fifteen at P1/pulse.

### Why `denom` is 0, and why that is not a regression

`denom` was designed as the coin's face value. **An S1 cannot produce it.** The
board has one wire from the coin acceptor and sees only pulses; it has no way to
distinguish a P5 from a P20. What the earlier firmware put in `denom` was
`price_per_credit_cents / 100` — a *configuration* value, identical on every row.
So `AnalyticsScreen`'s mixed breakdown (`P10 | 43 coins`, `P5 | 12 coins`) was
never reachable from real hardware; it comes from `seedDemo.ts`, which picks
denominations with weights.

Session rows carry `denom = 0` because a session has no face value, and 0 cannot
be mistaken for a real coin. That gives the app a clean filter.

### What the app needs to do

1. **Exclude `denom == 0` from the coin-denomination breakdown**, or drop that
   panel for S1 boards. Without this it renders a "P0 — 1 coin" row per session.
2. **Nothing else.** Revenue, `DailyStat` and the sync cursor all key off
   `amount` and `seq`, which are unchanged. `Session.denom` stays in the schema.
3. **Confirm 0 is the marker you want**, or give us the value/field you prefer —
   this is your allocation, and firmware will follow whatever you assign.

### One consequence worth stating plainly

Money is now recorded when it is **billed**, not when it enters the box. Coins
inserted but not yet worth a credit sit banked; the session that spends them
carries them. If the board resets first, that money is in the cash box and in no
row — bounded by one credit's worth of change, and tied to `R4`, which is open on
the firmware side.

## B4. [P3] Live Counters (Notify)

Coin value banked, credits, session state, relay state, time remaining. Enables
`A3` — the Diagnostics wizard's coin-path step becomes the sanctioned way to
confirm polarity, retiring the serial console from `H1`'s procedure.

## B5. [P3] Diagnostics / fault codes

`wdt_print_boot_report()` already classifies the reset reason at boot, so fault
`0x05` (watchdog reset since last sync) is mostly plumbing once `M5` exists.
Companions worth surfacing: LCD disabled after 3 failures, I2C recovery count
(`lcd_write_failures()`), coin-counter mutex failure (`R20`), stack high-water
(`R17`), and — per `D8` — **whether the board is unprovisioned**, which is
otherwise invisible to an operator until earnings fail to show up.

## B6. [P3] Command characteristic

`identify` (beep + LED), relay test, buzzer test, LED test, **`reboot`** — the last
one closes `A4` and makes the reboot-to-apply convention livable without physical
access. Must call `periph_reset_safe()` before `esp_restart()` (strapping pins —
`R15` is the related gap in the shutdown handler).

Every op needs the same "is this safe while vending?" gate: a relay test during a
paid session is a customer complaint.

**Do not put serial provisioning here** unless `D8` and `B9` both say so. A
`set_serial` op on an unbonded characteristic is strictly worse than the CLI path
it would replace, because it needs no physical access at all.

## B7. [P4] OTA

The default `esp32dev` partition scheme already has two app slots — **verify that
against the actual build** before planning around it, then no partition change is
needed. Interacts with `B9`: unauthenticated OTA over an unbonded link is a
worse door than the CLI backdoor `R22` describes.

Also relevant to `D2`: an OTA preserves the serial's NVS namespace, since it only
rewrites an app slot. `X7`'s `erase_flash` hazard is the same class of problem
though, and both belong in one short "what wipes identity" note.

## B8. [P4] WiFi provisioning + cloud check-in

**Do `X3` first.** WiFi + HTTP + JSON is what pushed the bring-up firmware to 94%
of flash. Current S1 build is ~49%; the headroom is probably there, but measure
before starting rather than after. And this is the item that carries a WiFi
password and a backend `device_key` over the link — so `B9` ships first.

## B9. [P2] Bonding / encryption

Contract specifies bonded, encrypted GATT (Just Works). Neither side implements
it and the app doesn't request it. **Priority is P2 rather than P4 because it must
precede `B8`**, and because the config characteristic being world-writable
(pricing, coin polarity) is the weakest part of the current "physical proximity"
security model. Pairs with `D5`.

---

# Cross-cutting engineering (X)

## X1. [P1] Centralize config validation

Bounds now live in **three** places with genuinely different values —
`app.cpp` (LCD menu), `cli.cpp` (CLI), `ble_config.cpp` (BLE). `relay_on_ms`
floors at 1000 on the LCD and 100 via the CLI; BLE picked the tighter one and said
so in a comment. `R18` adds a fourth gap: a config blob loaded from NVS is not
range-checked at all, and `R19` wants a `static_assert` on `sizeof(AppConfig)`.

Adding BLE was the forcing function `BLE_CONFIG_CONTRACT.md` open question #3
predicted. Do it before `B6` adds more writers: one validator in `config.cpp`,
called by all four paths (LCD, CLI, BLE, load). `M1`'s floor lands here.

Note the diffability rule — this touches `config.cpp`, which has an STM32
counterpart. Either mirror the change upstream or record the divergence.

## X2. [P2] Cross-task config apply path

`app.cpp` has no way to accept a new `AppConfig` from another task, which is the
sole reason BLE writes need a reset. A small mailbox — the app task picks up a
pending config at a safe point in its state machine, never mid-session — closes
`D6` properly and is the difference between the app's config UX feeling live or
feeling like a form that needs a power cycle.

## X3. [P3] Flash/RAM budget gate

**Measured 2026-09-09** (`pio run -e esp32dev`, `ENABLE_CLI` + `ENABLE_BLE`, clean
build): **flash 635,517 B / 48.5%** of the 1,310,720 B app partition, **RAM 36,276 B
/ 11.1%**. That settles the discrepancy — `BLE_CONFIG_CONTRACT.md`'s figure is
correct and the gap analysis's 643,472 (49%) is stale. **~675 KB free.**

Re-measure at each `B` item and keep this one number. Hard gate before `B8`
starts, since WiFi + HTTP + JSON is what pushed the bring-up firmware to 94%.

## X4. [P3] Record BLE as intended divergence

`CLAUDE.md`'s diffability rule says anything not enumerated in
`PORTING_FROM_STM32.md` §2/§3 should match the STM32. The entire BLE surface has
no upstream counterpart, so it belongs on that list explicitly — otherwise the
next person diffing the trees reads it as accidental drift. The serial from `D2`
is the same case: new NVS state with nothing upstream to diff against.

## X5. [P3] Stale comment

`src/main.cpp:118` still explains BLE task placement in terms of "the Bluedroid
stack's own tasks"; the build moved to NimBLE. The *conclusion* (BLE runs on core
0, app tasks on core 1) is unchanged — only the attribution is wrong.

## X6. [BUILT 2026-09-09] The serial lives outside `appcfg`

**Implemented.** `src/identity.h` / `src/identity.cpp`, NVS namespace `vendoid`,
key `serial` — read and written by nothing in `config.cpp`, so neither a factory
reset, nor a short or stale blob falling back to `config_defaults()`, nor a future
`APP_CONFIG_MAGIC` bump can erase or reassign a board's identity. It also means
this needed **no** magic bump and **no** change to the hardware-verified 37-byte
Config frame.

| Surface | |
|---|---|
| `identity_serial_valid()` / `_get()` | a stored value failing the format check reads as *unprovisioned*, not as a plausible identity — a garbled NVS entry must not present as a real board |
| `identity_serial_set()` | write-once per `D8`, enforced here rather than in the CLI, and read back after writing rather than trusting the write |
| `identity_serial_erase()` | token-gated, per `D8` |
| `identity_mac()` | `esp_efuse_mac_get_default` (canonical byte order, unlike `ESP.getEfuseMac()`'s reversed packing); zeroes the buffer on failure so a caller never sees stack garbage that looks like an address |
| `AT+SERIAL?` / `AT+SERIAL=` / `AT+SERIAL_ERASE?` / `AT+SERIAL_ERASE=` | `src/cli.cpp`. Note `AT+SERIAL_ERASE` is registered **before** `AT+SERIAL` — `dispatch()` takes the first prefix match |
| boot banner | `identity_print()` in `main.cpp`, so an unprovisioned board says so every boot |

Cost: **+3,372 bytes** flash (635,517 → 638,889, 48.5% → 48.7%), no measurable RAM
change.

**Verified on hardware 2026-09-13.** The first board was provisioned as
`VLABS-S1-00001` and `AT+SERIAL?` reads it back correctly along with the MAC, so
the NVS namespace, the format validation and the write path all work on a real
unit. **Still unexercised:** the write-once rejection, `AT+SERIAL_ERASE`, and NVS
surviving a reflash — worth five minutes on the same bench session.

**The standing rule this establishes, worth applying to `B3`'s `seq` counter:**
*identity and monotonic counters are not configuration.* Config is resettable by
design; these must not be.

## X7. [P1] Provisioning path vs `ENABLE_CLI`, and what wipes the serial

`D2` makes the CLI the provisioning tool; `D5`/`R22` want the CLI gone from field
units. Both can hold, but someone has to pick how:

- **Manufacture flashes a provisioning build (CLI on), provisions, then flashes
  production (CLI off).** Works — but the second flash must not erase NVS.
  `pio run -t upload` preserves it; **`esptool erase_flash` does not.** That
  belongs in the factory procedure in bold, because the failure mode is a board
  that ships with no identity and looks perfectly fine.
- **Keep `ENABLE_CLI` on and accept `R22`.** Simplest; leaves the free-vend
  backdoor open.
- **Provision over BLE.** Needs a write path and `B9` bonding first, and per `B6`
  is the *least* safe option until then — no physical access required.

Whichever is chosen, the "what wipes the serial" list (`erase_flash`, an NVS
partition resize, a partition-table change) wants to be one short documented note
rather than folklore.

---

# App / backend side (A)

Not this repo. Listed so each has an owner and a firmware dependency.

- **A1** — ~~re-key `Machine.deviceId`~~ **CLOSED 2026-09-14 by `D11`.** No fleet
  was claimed under the `VendoS1` placeholder, so there is nothing to migrate.
  Going forward the app keys on the serial, which it reads from the advertised
  name at scan time — no connection, no Device Info characteristic. The iOS
  half of the original problem is out of scope per `D10`.
- **A2** [P3] — explicit empty state on Dashboard/Analytics/Reports for
  `configOnly` machines. Today a permanent ₱0 is indistinguishable from "earned
  nothing today". No firmware dependency; can ship now.
- **A3** [P3] — polarity confirmation in the Diagnostics wizard. Needs `B4`.
- **A4** [P3] — reboot button. Needs `B6`.
- ~~**A5** [P1] — send the characteristic UUID allocation table.~~ **Closed
  2026-09-14** — no ask needed, the table was read straight from
  `mobile/src/ble/constants.ts` and is recorded in `BLE_CONFIG_CONTRACT.md`. The
  inference was right: `f001`–`f004` were already assigned. Both service UUIDs are
  final and the app scans for both.
- **A6** [P1] — **tell them `findBoardByDeviceId()` still hardcodes `'VENDOS1'`.**
  Only the reconnect *fallback*, so the symptom is "Board not found nearby" after a
  failed direct connect rather than a hard break. Their own comment that "every S1
  on a site looks identical over the air" is now stale — serial-as-name fixes it.
- **A7** [P0] — **agree a re-claim path before `B1` ships.** The app throws
  "Connected to the wrong board" when Device Info's `device_id` differs from the
  stored `machine.deviceId`, and every S1 claimed so far stored a BLE peripheral id
  there. Also confirm what an *unprovisioned* board should send as `device_id`,
  since they persist it as the durable identity.
- **A8** [P1] — **their Device Info payload is length-prefixed strings, ours was
  designed fixed-width.** We are adopting theirs (`B1`), so the earlier ask for a
  24-byte serial field is withdrawn. Confirm `hw_revision` has no home in their
  format and should be folded into `model`.

---

# Suggested sequence

Phases, not dates. Each phase is roughly "one thing at a time can land here".

**Phase 0 — unblock (days, mostly not code)**
~~`D8`~~ · ~~`X6`~~ · ~~`A5` UUID table~~ · `A7` re-claim path · `H1` coin
polarity · start `H2`

`D1`, `D2`, `D8`, `X6` and now `A5` are closed — identity is stored and
provisionable over the CLI, and the UUID allocation is known. **`H1` is now the
only firmware-side blocker in this phase**, and it is still one bench session that
everything money-related depends on.

> **Re-sequenced 2026-09-14.** `B1` turning out to be the app's capability probe
> (see `B1`) collapses what were separate phases: Device Info, Time Sync, Live
> Counters, Session Log and Diagnostics (`B1`–`B5`) must land as one release, so
> `B3`'s event log is on the critical path rather than later. The upside is that
> `firmware/esp32-bringup/src/main.cpp` in the app repo is a working reference
> implementation of all five, byte formats included.

**Also worth folding into the next bench session:** exercise `AT+SERIAL` on real
hardware. It has never run on a board — the write-once path, the erase token, and
NVS persistence across a reflash are all unverified, and `X6` is exactly the kind
of code where "compiles clean" says very little.

**Phase 1 — make the numbers trustworthy**
`M1`–`M4` (with `D3`, `D4`) · `X1` · finish `H2`

The gap analysis's step 0. Not BLE work; it decides whether BLE data is worth
collecting.

**Phase 2 — identity** — ~~mostly~~ **done, and the rest demoted**
~~`B1`~~ · ✅ `AT+SERIAL` · ✅ advertised name = serial · `X7` provisioning
procedure · ~~`A1` re-key~~

This phase was built around a data-integrity emergency that `D10`/`D11` dissolved.
What remains is `X7` — writing down the factory provisioning procedure, including
the `erase_flash` hazard. `B1` itself dropped to P3 and moved to Phase 4; nothing
waits on it.

**Phase 3 — the product's actual point, and now the front of the queue**
`B2` RTC + Time Sync · `D7` · `B3` event log + delta sync

Turns the board from a configuration target into the monitoring product it's
described as. With identity settled and `B1` demoted, this is the highest-value
BLE work left — subject to Phase 0/1 landing first, since an event log over
unverified billing produces numbers nobody should read.

**Phase 4 — supervision and the rest of the tree**
`M5` (via `R6`'s TWDT route, no `H3` dependency) · `B4` · `B5` · `B6` + `X2` ·
`B1` Device Info (demoted here from Phase 2)

`B4` retires the serial console from the polarity procedure; `B6` closes `A4`.

**Phase 5 — security, then reach**
`D5` (with `X7`) · `B9` bonding · `X3` budget gate · `B7` OTA · `B8` WiFi + cloud

Bonding precedes WiFi so credentials never cross an open link.

**Ongoing / anytime:** `H3`, `H4`, `X4`, `X5`, `A2`.

---

## Related

- `Gap-Analysis-App-vs-Firmware.md` — the app team's analysis this is derived from
- `docs/PENDING.md` — what the port deferred (`P1`–`P9`)
- `docs/REVIEW_FINDINGS.md` — what code review found (`R1`–`R22`)
- `docs/BLE_CONFIG_CONTRACT.md` — the 37-byte Config frame, the one thing verified
- `docs/PORTING_FROM_STM32.md` — the diffability rule and intended divergences
