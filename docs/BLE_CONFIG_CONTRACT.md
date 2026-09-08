---
tags: [firmware, ble, config, esp32]
updated: 2026-08-24
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

**⚠️ Service UUID is a placeholder.** `Board-Firmware-Contract.md` (the master doc that presumably defines
the service UUID and any Device Info characteristics) was not available when this was implemented — only
the Config characteristic UUID above was given. `BLE_SERVICE_UUID` in `ble_config.cpp` is a made-up value
that only follows the same custom 128-bit base. **Replace it with the real value before any real mobile
app tries to discover this board**, or discovery will simply fail. This does not block bench-testing with
nRF Connect below, since that tool discovers whatever GATT tree the board actually advertises.

**Flash cost — why NimBLE, not the built-in Bluedroid stack.** The first pass at this used the framework's
built-in `BLEDevice.h` (Bluedroid) and measured **~825 KB** of flash on top of this firmware's ~334 KB
baseline — 88.4% of the 1.31 MB app partition, leaving only ~150 KB of headroom for everything else this
board still needs to grow into (notably the master/slave daisy-chain protocol in `docs/PENDING.md` item 6).
Switching to NimBLE-Arduino for the identical GATT-server functionality (same 37-byte characteristic, same
validation, same live-apply exception for `coin_active_high`) dropped that to **635,517 bytes (48.5%)** —
roughly **190 KB** of BLE overhead instead of 825 KB. Measured by building with `-DENABLE_BLE` against
each library in turn; RAM cost is comparable between the two (~36–40 KB either way).

## Testing with nRF Connect for Mobile

1. **Flash the board.** From `Vendo_S1_ProductionFirmware`: hold BOOT (SW7), tap RESET (SW1), release
   BOOT, then `pio run -e esp32dev -t upload`. Tap RESET again to run the app. Open a serial monitor
   (`pio device monitor -e esp32dev`, 115200 baud) to watch the boot banner and any `BLE config write...`
   log lines during testing.
2. **Connect.** Open nRF Connect for Mobile (Android/iOS) → **Scanner** tab → find **`VendoS1`** → **Connect**.
3. **Find the characteristic.** Under **Client**, expand the one advertised service (UUID
   `6a400001-0000-1000-8000-00805f9b0001` — the placeholder above) → find characteristic
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
