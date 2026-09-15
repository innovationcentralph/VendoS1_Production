---
tags: [firmware, ble, config, esp32]
updated: 2026-09-15
---

# Board Config — BLE GATT Mapping

Maps the **real, shipping** `AppConfig` struct onto the BLE `Config`
characteristic (`6a40f005-0000-1000-8000-00805f9b0001`) defined in `Board-Firmware-Contract.md`, ahead of
the ESP32 port. This replaces that doc's placeholder `{time_per_peso, module_flags}` layout (3 bytes),
which was written before firmware's actual config model existed and doesn't match it — there is no single
"time per peso" field, and there is no generic module-enable bitmap (buzzer isn't optional; GSM is a single
unimplemented flag; door/vibration are not distinct concepts in code, just one generic sensor input).

> **2026-08-24 update:** the ESP32 port (`Vendo_S1_ProductionFirmware`) now exists and its `AppConfig`
> (`src/config.h:70-170`) has grown one field beyond the STM32 struct this doc was originally written
> against: `coin_active_high` (see field 20 below and **Open questions #5**). Everything else in this
> mapping was checked against the STM32 `main` branch (`src/config.h:49-133`) and the ESP32 port's
> `src/config.h` and still matches field-for-field — order, ranges, and defaults. Source of truth: STM32
> `struct AppConfig`, `src/config.h:49-133`, defaults `kDefaults` `src/config.cpp:18-39`; ESP32
> `struct AppConfig`, `src/config.h:70-170`, defaults `src/config.cpp`. LCD menu: `config_menu_run`
> (`app.cpp`, both platforms). CLI mirror: `src/cli.cpp` (`AT+...`, gated by `ENABLE_CLI`, both platforms).

## Wire format (binary, little-endian, fixed-width — same convention as the rest of the contract)

Config (Read/Write), **37 bytes total**:

| Offset | Bytes | Field | Type | Range | Default | LCD page? |
|---|---|---|---|---|---|---|
| 0 | 1 | `config_version` | u8 | schema guard, bump on layout change | 1 | — |
| 1 | 4 | `relay_on_ms` | u32 | 1,000–3,600,000 | 5000 | 4 |
| 5 | 4 | `coins_required` | u32 | 1–99 (pesos per credit) | 1 | 3 |
| 9 | 1 | `operation_mode` | u8 (enum) | 0,1,3 valid; 2,4 reserved | 0 | 0 |
| 10 | 1 | `press_to_start_per_credit` | bool | 0/1 | 0 | 2 |
| 11 | 2 | `inactivity_timeout_s` | u16 | 0–3600 (0=disabled) | 0 | 1 |
| 13 | 1 | `accumulation_enabled` | bool | 0/1 | 0 | 5 |
| 14 | 1 | `sensor_stop_enabled` | bool | 0/1 | 0 | 6 |
| 15 | 1 | `sensor_active_high` | bool | 0/1 | 0 | 7 (hidden unless page 6 ON) |
| 16 | 2 | `sensor_wait_ms` | u16 | 0–5000 | 50 | 8 (hidden unless page 6 ON) |
| 18 | 1 | `gsm_reporting_enabled` | bool | 0/1 | 0 | none — no LCD/CLI setter exists today |
| 19 | 1 | `display_type` | u8 (enum) | 0=none,1=LCD16x2,2=7-seg | 1 | none (CLI only) |
| 20 | 2 | `beep_start_freq_hz` | u16 | 100–20,000 | 1000 | none (CLI only) |
| 22 | 1 | `beep_start_count` | u8 | 1–10 | 2 | none (CLI only) |
| 23 | 2 | `beep_start_on_ms` | u16 | 10–5,000 | 150 | none (CLI only) |
| 25 | 2 | `beep_end_freq_hz` | u16 | 100–20,000 | 2500 | none (CLI only) |
| 27 | 1 | `beep_end_count` | u8 | 1–10 | 3 | none (CLI only) |
| 28 | 2 | `beep_end_on_ms` | u16 | 10–5,000 | 70 | none (CLI only) |
| 30 | 2 | `beep_off_ms` | u16 | 10–5,000 | 80 | none (CLI only) |
| 32 | 4 | `price_per_credit_cents` | u32 | 0–99,999 (centavos per raw pulse) | 1000 | 9 |
| 36 | 1 | `coin_active_high` | bool | 0/1 | **0 — pending bench confirmation, see below** | none (CLI only, `AT+COIN_POLARITY`) |

Corrects the previous revision of this doc, which stated "38 bytes total" but only listed 36 bytes of
fields (offset 32 + 4 bytes = ends at 36) — that arithmetic error is now resolved by both fixing the count
and accounting for the newly-added byte 36.

`config_version` here is a small BLE-facing schema guard, distinct from firmware's internal 32-bit
`magic` (STM32 `config.h:11`, `0xBEEFCB07`; ESP32 `config.h:30`, `0xBEEF5107` — the ESP32 value was
deliberately bumped specifically because of the `coin_active_high` addition below, so an old STM32-shaped
blob is rejected rather than misparsed) — `magic` gates the storage blob layout itself; `config_version`
just tells the app "this is the field layout I know how to parse," same spirit as Device Info's
`schema_version`.

## `operation_mode` enum (`OperationMode`, `config.h:26-35`)

| Value | Name | Implemented? | Behavior |
|---|---|---|---|
| 0 | Press to Start | Yes (default) | Press once → relay ON for full timer, button ignored while running |
| 1 | Pause/Resume | Yes | Button toggles relay ON/OFF; timer counts down regardless |
| 2 | ON/OFF Toggle | **No — reserved** | Spec: button toggles relay, timer pauses while OFF |
| 3 | Auto Start | Yes | No button — fires automatically once credit satisfied (500ms settle window) |
| 4 | Sensor Stop | **Not a mode — reserved value only** | Sensor Stop is the composable `sensor_stop_enabled` flag, not a mode |

**A BLE write handler must validate against `kImplementedOpModes[] = {0, 1, 3}` (`config.cpp:9-13`),
not the raw enum range** — writing 2 or 4 would silently fall through to undefined FSM behavior, the
same trap the firmware comments warn LCD/CLI implementers about.

## `display_type` enum (`DisplayType`, `config.h:16-20`)

| Value | Name |
|---|---|
| 0 | None |
| 1 | LCD 16x2 (I2C, PCF8574 backpack, addr 0x27) — default |
| 2 | TM1637 4-digit 7-segment |

## Composable flags (not mutually exclusive, layer on top of `operation_mode`)

- **Per Credit** (`press_to_start_per_credit`) — only meaningful for modes 0/1; LCD hides this page otherwise.
- **Accumulation** (`accumulation_enabled`) — mid-session coins add extra time instead of being ignored; also keeps the coin slot live during a session.
- **Sensor Stop** (`sensor_stop_enabled` + `sensor_active_high` + `sensor_wait_ms`) — session also ends early on the generic sensor input (PB0 on STM32, AUX_1/IO32 on the S1). No distinct door/vibration concept in firmware — same physical pin, same three fields, whatever's wired to it.
- **Coin polarity** (`coin_active_high`, ESP32/S1 only) — which level on the coin input counts as a pulse. Money-critical: see **Open questions #5**.

## Fields with no current LCD or CLI setter

`gsm_reporting_enabled` has no way to be set today except a direct EEPROM/NVS patch or reflash — BLE would
be the first real writer for it. The GSM report itself is a stub (`// TODO: gsm_report_send()`), so
writing this flag currently has no observable effect on real hardware.

## Open questions for the ESP32 Config characteristic (need a decision before firmware locks the layout)

1. **Live-apply vs. reboot-to-apply.** LCD writes apply immediately in RAM on both platforms. CLI writes on
   both platforms persist immediately but print `"NOTE: send AT+RESET to apply"` and require a reset to take
   effect. BLE should almost certainly match the LCD's live-apply behavior — confirm with firmware.
2. **One blob vs. split characteristics.** All 37 bytes fit in a single GATT write even at default MTU 23
   (fragmented) — no technical need to split. But grouping could mirror the LCD page order (core vend
   behavior) vs. CLI-only tuning (beeps/display/GSM) if the app wants to show/hide sections. Recommend
   single characteristic unless the app team wants that UI split.
3. **Per-field range validation** currently lives twice (`app.cpp` for LCD, `cli.cpp` for CLI) with slightly
   different bounds in places (e.g. `relay_on_ms` min is 1000 on LCD, 100 via CLI). Adding BLE as a third
   writer is a good forcing function to centralize validation in `config.cpp` once, so all three paths
   enforce identical bounds.
4. **`sensor_stop_enabled`/`sensor_active_high`/`sensor_wait_ms`** currently model one generic sensor pin.
   If the ESP32 revision wants distinct door + vibration sensors (per the hardware capabilities table in
   the main contract), this needs two independent sets of these three fields, not one — worth deciding
   before the struct is duplicated into flash layout.
5. **`coin_active_high`'s correct value is still unconfirmed on real hardware** (`Vendo_S1_ProductionFirmware/docs/PENDING.md`
   item 3, [HIGH — money]). The S1's coin-pulse opto (PC817, U4) has unknown output polarity per the
   schematic, so this field ships with a **guessed default** (`false` = idle HIGH, pulse LOW) pending a
   bench check with `AT+COIN?` / `AT+COIN_POLARITY`. The field is well-defined and stable at byte offset 36
   regardless of what that bench check finds — only the *default value*, not the *layout*, is pending. Do
   not treat a BLE-set value for this field as authoritative until that bench confirmation has happened on
   the target board.

## Implementation (2026-08-24)

Lives in `src/ble_config.cpp` / `src/ble_config.h`, gated behind `-DENABLE_BLE` in `platformio.ini` the
same way the CLI is gated behind `-DENABLE_CLI` — drop the flag to remove the GATT server (and its flash
cost, see below) from a production build. Uses **NimBLE-Arduino** (`h2zero/NimBLE-Arduino@^1.4.1` in
`lib_deps`), not the framework's built-in Bluedroid `BLEDevice.h` — see below for why. Pinned to the 1.x
line because this platform's `framework-arduinoespressif32` package is the Arduino-ESP32 2.x/IDF4 line,
and NimBLE-Arduino 2.x targets IDF5/Arduino-ESP32 3.x.

**Behavior implemented:**
- `onRead()` re-reads the stored config from NVS and serializes it to the 37-byte wire format on every
  read — never serves a stale cached value.
- `onWrite()` validates the incoming 37 bytes against the exact bounds in the table above (length,
  `config_version == 1`, every range, `operation_mode` against `kImplementedOpModes[]`). Any failure is
  logged to `Serial` with the specific reason and the write is dropped entirely — nothing partial is
  persisted. A follow-up read always reflects NVS truth, never the rejected bytes.
- **Apply timing resolves open question #1 by matching the CLI, not the LCD**, for every field except
  `coin_active_high`: a valid write is persisted immediately (`config_save()`), but only takes effect in
  the running app task after `AT+RESET` or a power cycle — identical to every CLI setter in `cli.cpp`.
  `coin_active_high` is the one exception, applied live via `coin_counter_set_polarity()`, mirroring the
  CLI's own `AT+COIN_POLARITY` special-case for the same reason (it's a bench-calibration value, and
  waiting for a reset to learn whether a guess was right would make bench-testing painfully slow).
  Reaching into the running app task's live `AppConfig` for the *other* fields (true LCD-style live-apply)
  would need a cross-task synchronization path that does not exist in `app.cpp` today — worth building if
  BLE needs true live-apply later, but out of scope for this first pass.

**Service UUID — ratified 2026-09-09, no longer a placeholder.** `BLE_SERVICE_UUID` in `ble_config.cpp`
(`6a400001-0000-1000-8000-00805f9b0001`) was originally a value invented here, because
`Board-Firmware-Contract.md` was not available to this repo and only the Config *characteristic* UUID had
been given. The app was then built and hardware-verified against it, so the decision (`docs/APP_BLE_PLAN.md`
D1) was to **adopt it as the contract** rather than churn a working, deployed pairing. Nothing to replace.

Two notes for whoever extends the GATT tree:

- The tail `0000-1000-8000-00805f9b0001` resembles the Bluetooth SIG base UUID (`...00805f9b34fb`) but
  differs in the final group, so it cannot collide with a SIG 16-bit UUID and no stack will shorten it.
  It only *looks* SIG-ish; it is a deliberate custom value.
- **Config is `6a40f005`, which implies `Board-Firmware-Contract.md` already allocates `f001`–`f004`**
  (and probably beyond). Get that allocation table from the app team before minting a UUID for Device
  Info, Time Sync, Event Log, Live Counters, Diagnostics, Command, OTA or WiFi — two sides independently
  picking the next free number is how a collision ships. Tracked as `APP_BLE_PLAN.md` A5.

**Flash cost — why NimBLE, not the built-in Bluedroid stack.** The first pass at this used the framework's
built-in `BLEDevice.h` (Bluedroid) and measured **~825 KB** of flash on top of this firmware's ~334 KB
baseline — 88.4% of the 1.31 MB app partition, leaving only ~150 KB of headroom for everything else this
board still needs to grow into (notably the master/slave daisy-chain protocol in `docs/PENDING.md` item 6).
Switching to NimBLE-Arduino for the identical GATT-server functionality (same 37-byte characteristic, same
validation, same live-apply exception for `coin_active_high`) dropped that to **635,517 bytes (48.5%)** —
roughly **190 KB** of BLE overhead instead of 825 KB. Measured by building with `-DENABLE_BLE` against
each library in turn; RAM cost is comparable between the two (~36–40 KB either way).

## UUID allocation — RESOLVED 2026-09-14 (read from the app's source)

The blocker "firmware must not invent UUIDs" is **closed**. The allocation was read
directly out of the app repo, which is now in this workspace:

- **`c:/vendoLabs_Repos/app/vendolabsapp/mobile/src/ble/constants.ts`** — the compiled-in
  constants (`CHARACTERISTICS`), the authority for what the app will actually look up.
- **`VendoLabs PH/Wiki/Board-Firmware-Contract.md`** — the app team's contract doc that
  `constants.ts` says it "mirrors exactly".

**Where the values originated** (the open question in `HANDOFF.md`): not
`Vendo_S1_TestCode`. They were defined by the **app team**, and are implemented by
`firmware/esp32-bringup/src/main.cpp` — a 1340-line PlatformIO ESP32 reference firmware
*inside the app repo* that implements all 13 characteristics. That tree is the
"Bring-up FW ✅" column in `Gap-Analysis-App-vs-Firmware.md`. The gap analysis was not
wrong; it was pointing at a different tree than the one we assumed.

### The allocation table

Base is `-0000-1000-8000-00805f9b0001` for every entry; only the first group varies.
These are a private vendor UUID space, not SIG-registered.

| Characteristic | UUID first group | Props | Status here |
|---|---|---|---|
| **Service (contract)** | `6a40f000` | — | **not ours** — see below |
| **Service (Vendo S1)** | `6a400001` | — | ✅ shipping, final |
| Device Info | `6a40f001` | Read | ⚠️ built, **OFF by default** — see the trap below |
| Time Sync | `6a40f002` | Write | ✅ built |
| Live Counters | `6a40f003` | Read/Notify | ✅ built **and hardware-verified** (pesos since `3fba2e9`) |
| Session Log | `6a40f004` | Read/Write | ✅ built **and hardware-verified** 2026-09-15 (`src/ble_sessionlog.cpp`) |
| **Config** | `6a40f005` | Read/Write | ✅ shipping |
| Diagnostics | `6a40f006` | Read/Notify | ✅ built **and hardware-verified** |
| Command | `6a40f007` | Write | ✅ built 2026-09-15 (`src/ble_command.cpp`) — all 7 ops |
| OTA Control | `6a40f008` | Write | ❌ |
| OTA Data | `6a40f009` | Write | ❌ |
| OTA Status | `6a40f00a` | Read/Notify | ❌ |
| WiFi Config | `6a40f00b` | Write | ❌ |
| WiFi Status | `6a40f00c` | Read/Notify | ❌ |
| OTA WiFi Begin | `6a40f00d` | Write | ❌ |

**The `f005`-implies-`f001`–`f004` inference was correct.** Config really was allocated
as the fifth member of a pre-existing scheme.

### Two service UUIDs, both final, neither to be changed

`6a40f000` is the contract's service, implemented by the app-repo bring-up firmware.
`6a400001` is ours — it began as a placeholder here and the app team has since ratified
it as final on their side too (`KNOWN_SERVICE_UUIDS` scans for both). This confirms
decision `D1` from the outside. **Do not "adopt" `6a40f000-…`**: the app distinguishes
the two firmwares by GATT tree, not by service UUID, so switching ours has no upside and
would invalidate every flashed board.

### Config frame: verified byte-for-byte

`mobile/src/ble/codec.ts` (`decodeConfig`/`encodeConfig`, `CONFIG_BUFFER_SIZE = 37`) was
diffed against `serialize_cfg`/`deserialize_cfg` in `src/ble_config.cpp`. **All 20 fields
match — offsets, widths, little-endian, and the trailing `coin_active_high` at offset 36.**
The app explicitly documents that it tracked our 37-byte frame rather than the contract's
older 38-byte text. Nothing to change.

Field ranges also agree, with **one deliberate asymmetry**: the app clamps
`pricePerCreditCents` to a floor of **1**, we accept **0**. That is `R1` — the app closed
it client-side and its comment cites our `REVIEW_FINDINGS.md` R1 by name. The app refusing
to send 0 is not a substitute for firmware rejecting it; `R1` stays open here.

### Payload layouts for the unimplemented characteristics

Taken from `codec.ts`, little-endian, so these need no further negotiation:

- **Device Info** (Read): `schema_version:u8`, `modules_bitmap:u8`, then three
  length-prefixed UTF-8 strings — `device_id`, `model`, `fw_version` (each `len:u8` + bytes).
  `schema_version` must be **1**; the app refuses to sync on any other value, newer *or* older.
  Module bits: door 0, vibration 1, buzzer 2, gsm 3.
- **Time Sync** (Write): `epoch_utc:u32`. Confirms the Time Sync policy proposed in
  `HANDOFF.md` — UTC epoch, written first on every connect.
- **Live Counters** (Read/Notify, 14 B): `today_amount:u32`, `today_sessions:u16`,
  `lifetime_amount:u32`, `last_seq:u32`. **Money here is PESOS** — see the units note below; sending
  centavos was a real 100x bug, fixed in `3fba2e9`. `today_sessions` counts **paid periods**, not coins.
  Both figures roll over on the **Asia/Manila** business day (16:00 UTC), matching `businessDay.ts`.
- **Session Log** (Read/Write): write cursor `after_seq:u32` (**exclusive**, on every page);
  read back `count:u8`, `has_more:u8`, then `count` × 14-byte entries of
  `seq:u32`, `ts:u32`, `ts_unverified:u8`, `denom:u8`, `amount:u32`.
- **Diagnostics** (Read/Notify): `uptime:u32`, `boot_ts:u32`, `boot_ts_unverified:u8`,
  `sensors_bitmap:u8` (door 0, vibration 1, coin 2, rtc 3), `errors_count:u8`, then that
  many `u8` codes. Codes: `0x01` coin pulse line fault, `0x02` relay drive fault,
  `0x03` RTC unset / battery-backup failure, `0x04` EEPROM write failure,
  `0x05` watchdog reset since last sync.
- **Command** (Write, 5 B): `op:u8`, `param:u32`. Ops: `0x01` syncAck, `0x02` clearErrors,
  `0x03` identify, `0x04` testRelay, `0x05` testBuzzer, `0x06` testLed, `0x07` wifiForget.
  **Implemented 2026-09-15.** Three notes the app should know:
  - **Physical ops are refused while a session is running** (`identify`, `testRelay`, `testBuzzer`,
    `testLed`). A relay click mid-vend is a customer complaint. `syncAck`, `clearErrors` and
    `wifiForget` run at any time — refusing a `syncAck` mid-vend would fail a connect for no reason.
  - **A refusal is invisible to you.** The characteristic is write-only and the contract gives it no
    status channel, so the ATT write succeeds either way. If the Diagnostics wizard needs to know, that
    needs a field from your side — see `D20` in `APP_BLE_PLAN.md`.
  - **`syncAck` is recorded, never applied.** The board keeps its own `last_seq`; writing the backend's
    value into it could rewind sequence numbers (a partial sync, a restored backup, a second phone).
  - **`wifiForget` is accepted and ignored** — there is no WiFi on this board yet.
  - **`reboot` has no op code.** `0x01`–`0x07` are all taken and firmware will not invent `0x08`; the
    allocation is yours. It is what `A4` needs.

MTU: the app requests 247 but the contract requires firmware to still work at the default
23, paging in smaller pages.

### ⚠️ Units on the wire — the contract is internally inconsistent, deliberately documented

| Characteristic | Field | Unit |
|---|---|---|
| `f003` Live Counters | `today_amount`, `lifetime_amount` | **PESOS** |
| `f004` Session Log | `amount`, `denom` | **PESOS** |
| `f005` Config | `price_per_credit_cents` | **CENTAVOS** |
| `f005` Config | `coins_required` | whole **PESOS** (price of one credit) |

Firmware follows this exactly; internally it is centavos everywhere and converts once, at the wire. The
conversion is exact rather than lossy because price-per-pulse is always a whole number of pesos.

**This cost a real bug.** Before `3fba2e9`, Live Counters sent centavos: a board that had taken P50
reported `5000`, and the app renders these with no division. **Any backend data synced from a board
flashed before that commit is inflated 100x.** Recorded here so the next characteristic that carries money
gets the decision made deliberately instead of inherited.

### Session Log as implemented (2026-09-15) — what the app will actually meet

**Verified on a board, byte-exact.** Three sessions were recorded and read back over BLE at cursor 0:

```
write 00000000        (after_seq = 0)
read  03 00 | 01 00 00 00 00 00 00 00 01 00 01 00 00 00
              02 00 00 00 00 00 00 00 01 00 02 00 00 00
              03 00 00 00 00 00 00 00 01 00 04 00 00 00

count=3  has_more=0   sessions of P1 / P2 / P4, ts_unverified=1 (bench clock unset)
```

Unverified as of this revision: pagination past the first page, persistence across a power cut, and the
wrap at ~8,100 rows.


- **Page size is 10 events = 142 bytes.** That exceeds a 23-byte MTU on purpose:
  the page is built once, when the cursor is written, so an ATT long read
  collects a stable buffer and a coin arriving mid-read cannot change the page's
  length underneath the client. At the negotiated 247 no long read happens at all.
  The app team's bring-up firmware pages 1 event at a time; at that size, its own
  `MAX_SESSION_LOG_PAGES` cap of 500 would limit a connect to 500 events.
- **One row is one SESSION — one paid period**, carrying the total billed for it
  in **pesos** and stamped when its first credit was billed (changed 2026-09-15;
  it was one row per coin pulse before). A customer inserting P20 then P100 into
  the same run is one row of P120. Row count and Live Counters' `today_sessions`
  therefore now mean the same thing.
- **🔴 ACTION FOR THE APP: `denom` is 0 on every row and the breakdown must
  filter on that.** A session has no coin face value, and the board cannot know
  one anyway — it sees pulses on one wire and cannot tell a P5 from a P20. Until
  `AnalyticsScreen` excludes `denom == 0` from its coin breakdown, it renders a
  "P0 — 1 coin" row per session. **Revenue is unaffected**: gross, `DailyStat`
  and the sync cursor key off `amount` and `seq`.

  Worth knowing before objecting: that breakdown was never reachable from an S1.
  The previous firmware put `price_per_credit_cents / 100` in `denom` — a config
  value, identical on every row — so the mixed `P1/P5/P10/P20` spread only ever
  came from `seedDemo.ts`. **If 0 is not the marker you want, tell us what is:
  the field is your allocation and firmware will follow.** Full ask in `D18`,
  `docs/APP_BLE_PLAN.md`; app-facing summary as `A8` in
  `Gap-Analysis-App-vs-Firmware.md`.
- **Retention is ~8,100 rows** in a dedicated 128 KB flash partition, wrapping
  oldest-first — months of sessions. A client further behind gets the oldest row,
  and the gap is visible to the backend as `firstSeq > lastSeq + 1` — there is no
  "you missed rows" field on this wire and none was invented. **Worth raising
  with the app team**, since nothing acts on it yet.
- **A full ring needs two connects to drain**: 500 pages × 10 rows = 5,000 rows
  per sync against a capacity of 8,192. Far less likely now that a row is a
  session rather than a pulse.
- **The cursor is per connection**, released on disconnect, so two phones pulling
  at once cannot corrupt each other's pagination.

### ⚠️ Device Info cannot ship alone — it is a capability probe

`BleService.resolveCapabilities()` decides a board's profile with a single test:

```ts
const profile = uuids.includes(CHARACTERISTICS.deviceInfo) ? 'full' : 'configOnly';
```

Today an S1 is `configOnly`, and the app **skips the whole sync flow** and goes straight
to Config — which is why Config works. **The moment we expose `6a40f001`, the same board
becomes `full`**, and `BleConnectionContext` runs the full sequence instead:

`writeTimeSync` → `readDeviceInfo` → schema check → `readLiveCounters` → `readConfig`
→ `pullSessionLogDelta` → `readDiagnostics` → `readWifiStatus` (this one is caught) → upload.

The first missing characteristic throws and the connect fails — **including the Config
push that works today.** So adding Device Info on its own is not an increment, it is a
regression.

**Therefore `f001`, `f002`, `f003`, `f004` and `f006` must land in one release, or none of
them.** **And so must `f007`** (all six built as of 2026-09-15) — an earlier revision of this doc said Command was "gated
separately in the UI and can wait", which reading `BleConnectionContext.tsx` disproves:
`acknowledgeSync()` is called unconditionally, with no `.catch()`, two lines after a
`readWifiStatus()` that does have one. Without `f007` the connect throws **after** the
backend upload has already succeeded, so the data lands server-side, the app reports a
failure, and `sync_ack` never reaches the board. The OTA/WiFi set genuinely can wait.
This is a hard sequencing constraint on `B1`/`B3` in `docs/APP_BLE_PLAN.md`, and it argues
against reading `B1`'s demotion to P3 as "do it whenever" — it is not independently
shippable in either direction.

### ⚠️ `device_id` mismatch will reject already-claimed machines

On a `full` board the app asserts:

```ts
if (deviceInfo.deviceId !== machine.deviceId) throw new Error('Connected to the wrong board — device ID did not match.');
```

An S1 claimed today stores the **BLE peripheral id (MAC on Android)** as `machine.deviceId`,
because there is no Device Info to read a real one from. The first time we report
`VLABS-S1-00001` there instead, every machine already claimed in an app install fails with
*"Connected to the wrong board"*. `D11` established there is no *backend fleet* to migrate;
this is a different thing — per-install app records — and needs an app-side re-claim or
migration path agreed before Device Info ships.

### Advertised-name change: primary path safe, fallback degraded

The app still has `S1_ADVERTISED_NAME = 'VENDOS1'` and matches it in two places. Effect of
our change to `VLABS-S1-00001`:

- **Discovery — fine.** `isVendoBoard()` matches on service UUID first, and we keep the
  UUID in the advertisement proper. The name test is only a fallback.
- **Reconnect — fine in the normal case.** `connectKnownBoard()` connects directly by
  peripheral id; for an S1 that id *is* the stored `deviceId`.
- **Reconnect fallback — broken.** If the direct connect fails, `findBoardByDeviceId()`
  requires `name === 'VENDOS1'`, which no longer matches. The user-visible symptom is
  *"Board not found nearby"* instead of a successful retry.

Low severity, but it is a real one-line change on the app side and they should be told
rather than left to discover it. Also note their comment "every S1 on a site looks
identical over the air" is now **stale in our favour** — serial-as-name fixes exactly the
complaint they wrote up.

## Testing with nRF Connect for Mobile

1. **Flash the board.** From `Vendo_S1_ProductionFirmware`: hold BOOT (SW7), tap RESET (SW1), release
   BOOT, then `pio run -e esp32dev -t upload`. Tap RESET again to run the app. Open a serial monitor
   (`pio device monitor -e esp32dev`, 115200 baud) to watch the boot banner and any `BLE config write...`
   log lines during testing.
2. **Connect.** Open nRF Connect for Mobile (Android/iOS) → **Scanner** tab → find **`VendoS1`** → **Connect**.
3. **Find the characteristic.** Under **Client**, expand the one advertised service (UUID
   `6a400001-0000-1000-8000-00805f9b0001` — ratified, see above) → find characteristic
   `6a40f005-0000-1000-8000-00805f9b0001`, shown with **R** (read) and **W** (write) badges.
4. **Read it.** Tap the down-arrow / read icon. You should get back 37 bytes matching the board's current
   stored config. With factory defaults, that's:
   ```
   01 88 13 00 00 01 00 00 00 00 00 00 00 00 00 00 32 00 00 01 E8 03 02 96 00 C4 09 03 46 00 50 00 E8 03 00 00 00
   ```
   (`config_version=1, relay_on_ms=5000, coins_required=1, operation_mode=0 (Press to Start), ...,
   sensor_wait_ms=50, display_type=1, beep_start=1000Hz×2@150ms, beep_end=2500Hz×3@70ms, beep_off=80ms,
   price_per_credit_cents=1000, coin_active_high=0`.)
5. **Write a change.** Tap the up-arrow / write icon, switch the value type to **BYTE ARRAY** (hex), and
   paste a modified 37-byte frame. For example, this changes `relay_on_ms` to 8000 ms and `coins_required`
   to 2, leaving everything else at default:
   ```
   01 40 1F 00 00 02 00 00 00 00 00 00 00 00 00 00 32 00 00 01 E8 03 02 96 00 C4 09 03 46 00 50 00 E8 03 00 00 00
   ```
   Tap **Send**. The serial monitor should print `BLE config write OK — stored. Send AT+RESET or
   power-cycle to apply the rest.`
6. **Verify it landed.** Read the characteristic again — it should now echo back the new 37 bytes (this
   confirms persistence without needing the serial console at all). To see it actually change the vend
   behavior, send `AT+RESET` over serial (or power-cycle) and check `AT+CFG?` — `relay_on_ms` and
   `coins_required` should show the new values.
7. **Test the live-apply exception.** Flip byte 36 (`coin_active_high`) between `00` and `01` and write.
   Unlike every other field, `AT+COIN?` over serial should show the new polarity **immediately**, no
   reset needed — this is the one field the BLE write applies live, matching `AT+COIN_POLARITY`'s existing
   behavior.
8. **Test rejection.** Write something invalid — e.g. change byte 0 (`config_version`) to `02`, or set
   bytes 2-5 (`relay_on_ms`) to a value under 1000 ms, or byte 9 (`operation_mode`) to `02` (reserved,
   unimplemented). The serial monitor should print the specific rejection reason, and a follow-up read
   should show the *previous* stored value is untouched — nothing partial should ever be visible.
