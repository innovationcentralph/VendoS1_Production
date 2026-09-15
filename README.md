# Vendo S1 — Production Firmware (ESP32)

Vending control firmware for the **ESP32-WROOM-32 Vendo S1 board**: coin billing,
session/relay timing, three operation modes, a 16x2 LCD config menu and an AT+
serial CLI.

This is a port of the STM32F103C8 VendoBoard production firmware
(`c:\vendo\Vendo_Production\VendoBoard_ProductionFirmware`, branch `main` @
`ee8e253`) and is at **behaviour parity** with it, with one user-facing
difference: the config menu uses two buttons instead of three, because that is
what the board has. See [`docs/PORTING_FROM_STM32.md`](docs/PORTING_FROM_STM32.md).

> **Nothing here has run on hardware yet.** It compiles clean, and the logic is a
> faithful port of firmware that works — but every bench item in
> [`docs/PENDING.md`](docs/PENDING.md) is still open, and item 3 (coin pulse
> polarity) touches money. Read that file before flashing a board you care about.

Related:

| | |
|---|---|
| [`docs/PORTING_FROM_STM32.md`](docs/PORTING_FROM_STM32.md) | What changed from the STM32 and why; pin mapping; inherited bug fixes |
| [`docs/PENDING.md`](docs/PENDING.md) | Open items — **watchdog supervision** and **coin polarity** first |
| `../Vendo_S1_TestCode` | Bring-up harness. Prove the board with this *before* running product firmware |
| `../Vendo_I2C_Master_Slave/docs/hardware/ESP32_Vendo_Board.md` | Schematic reference — the source of truth for every pin |

## Build and flash

```
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COMx
pio device monitor -e esp32dev
```

J7 (PROG) is a plain 4-pin UART header — pin1 +5V, pin2 `ESP_TX`, pin3 `ESP_RX`,
pin4 GND — with **no RTS/DTR auto-reset network**, so flash mode is manual (the
same shape as the STM32 rig's BOOT0 procedure):

1. hold **BOOT** (SW7)
2. tap **RESET** (SW1)
3. release BOOT
4. `-t upload`
5. tap RESET again to run the app

Two build flags in `platformio.ini`, both on by default for dev builds:

- `VERBOSE_LOG` — chatty per-coin/per-tick traces. Commented out by default.
- `ENABLE_CLI` — the AT+ parser. Dropping it for production also removes the only
  way to set beep tones, display type, coin polarity, or factory-reset the config
  without reflashing, since none of those have an LCD page.

## How it vends

Money-first billing. `coins_required` is the **peso price of one credit**, not a
pulse count:

```
Coins inserted   = raw pulses x price_per_credit_cents
Credits          = floor(coins inserted / (coins_required x 100))
Relay ON time    = credits x relay_on_ms
```

Leftover cents that do not complete a credit are **never discarded** — they stay
banked for the next credit or the next session.

### Operation modes

| Mode | Behaviour |
|---|---|
| **0 — Press to Start** *(default)* | Insert credit, tap START, relay runs the whole billed duration in one stretch. Button ignored while running. |
| **1 — Pause/Resume** | Same start, but START then toggles the relay off/on. **The timer keeps running either way** — pausing does not preserve time (per spec). |
| **3 — Auto Start** | No button. Fires by itself once credit is satisfied, after a 500 ms settle window so a whole deposit bills together. |

Mode 2 (ON/OFF Toggle, a pause that also freezes the timer) is a reserved enum
value on both platforms and is **not selectable** — you should never reach it.

### Composable features

These are not modes; each combines with any of the three above.

| Feature | Effect |
|---|---|
| **Per Credit** *(modes 0/1 only)* | Dispense one block per button press instead of all at once. LCD shows credits remaining between presses. |
| **Inactivity Timeout** *(modes 0/1 only)* | One shared timer: auto-starts if credit is ready but nobody presses, and auto-advances between per-credit blocks. 0 = wait forever. |
| **Accumulation** | Coins inserted mid-session extend the running time live. Also forces the coin slot to stay enabled for the session — there would be nothing to accumulate otherwise. OFF inhibits the acceptor for the whole session. |
| **Sensor Stop** | An active session also ends early when a sensor triggers. Configurable polarity and a required continuous-active time (filters flicker). Reads **J6 AUX_1 (IO32)** — see `PENDING.md` item 5. |

## Config menu (LCD)

**The gestures differ from the STM32**, which had a third button this board does
not break out:

| | |
|---|---|
| **BTN1 + BTN2 long press** (~1.5 s) | From idle: **enter** config. In the menu: **SAVE** all pages and exit. |
| **BTN1** / **BTN2** | Adjust the value on the current page. Hold to auto-repeat, then accelerate. |
| **START tap** | Next page (wraps; hidden pages are skipped). |
| **START long press** (~1.5 s) | **CANCEL** — discard every change and exit. |

Nothing is written until you save, so a power cut mid-menu is equivalent to
cancelling. **The coin slot is force-disabled for the whole menu**, whatever the
other settings say.

Ten pages, in order: Operation Mode, Inactivity Timer, Per Credit, Coins per
Credit, Time per Credit (relay), Accumulation, Sensor Stop, Sensor Polarity,
Sensor Wait Time, Price per Pulse. Pages that cannot apply to the selected mode
are skipped automatically — Inactivity Timer and Per Credit only appear for modes
0/1, and the two sensor detail pages only when Sensor Stop is on.

## Serial CLI

115200 8N1 on J7. `AT+HELP?` lists everything. Note that CLI setters edit the
**stored** config and print `NOTE: send AT+RESET to apply` — the LCD menu is the
path that applies immediately.

Most useful at bring-up:

| | |
|---|---|
| `AT+CFG?` | Every config field, parsed |
| `AT+EEPROM?` | Raw stored bytes + parsed fields |
| `AT+COIN?` | **Live coin input** — level, polarity, pulse count. Drop a coin; the count must rise |
| `AT+COIN_POLARITY=<0\|1>` | Flip the counting polarity. **Applies live, no reset** — see `PENDING.md` item 3 |
| `AT+WDT?` | External watchdog status, pet count, liveness ages |
| `AT+STRAP?` | JP10 role strap position + **why the board last rebooted** |
| `AT+LCD?` | Display health, I2C address, recovery count |
| `AT+CREDITS?` | Credits and money currently banked |
| `AT+EEPROM_RESET` | Factory defaults |

**Board identity, clock and earnings** — ESP32-only, no STM32 counterpart. Note
these are *not* config: they live in their own NVS namespaces, so a factory reset
clears settings without touching a board's identity or its takings.

| | |
|---|---|
| `AT+SERIAL?` | The board's serial and MAC, or `UNPROVISIONED` |
| `AT+SERIAL=VLABS-S1-NNNNN` | Provision the serial. **Write-once** — this is the app's `device_id` |
| `AT+SERIAL_ERASE?` | The erase token for *this* board (last 3 MAC bytes) |
| `AT+SERIAL_ERASE=<token>` | Clear the serial for RMA/refurb. Token-gated so a pasted command cannot orphan the wrong unit |
| `AT+RTC?` | Clock status — chip time, system time, drift, validity, trickle register |
| `AT+RTC=<epoch>` | Set the clock from epoch seconds **UTC** |
| `AT+RTC=YYYY-MM-DD HH:MM:SS` | Same, in a form a human can type. UTC, clamped to 2026–2050 |
| `AT+COUNTERS?` | Earnings totals — today, sessions, lifetime. Same data as BLE Live Counters |
| `AT+LOG?` | Event log health — depth, seq range, wraps, drops |
| `AT+LOG=<rows>[,<after_seq>]` | Dump events. `AT+LOG=10,0` is the first page the app would pull on a machine it has never synced |
| `AT+SYNC?` | Whether the app has ever confirmed a sync, and up to which seq |

`AT+COUNTERS?` is the bench version of the app's coin-path test: note `lifetime`,
drop a coin, run it again. If it moved, the coin path and the configured polarity
are both working.

`AT+LOG?` is the same idea for the event log, with one difference worth knowing:
**one row is one completed session**, not one coin. Coins insert, the relay runs,
the period ends — that whole thing is a single row carrying the total billed.
Money sitting banked (inserted, not yet spent) is in the box but not yet in any
row. Amounts are **pesos**, because that is what the BLE wire carries — the rest
of the firmware is in centavos.

## BLE

A GATT server (NimBLE), gated behind `-DENABLE_BLE`. The board advertises **its
serial as the device name** — `VLABS-S1-00001`, or `VLABS-UNSET-xxxx` before it is
provisioned — so a cabinet of machines is distinguishable in a scanner without
connecting. **Clients must discover by service UUID**, not by name.

Service `6a400001-0000-1000-8000-00805f9b0001`:

| UUID | Characteristic | Props | State |
|---|---|---|---|
| `6a40f001` | Device Info | Read | built, **off by default** — see below |
| `6a40f002` | Time Sync | Write | built |
| `6a40f003` | Live Counters | Read/Notify | built, hardware-verified |
| `6a40f004` | Session Log | Read/Write | built, **never run on hardware** — one row per session |
| `6a40f005` | Config | Read/Write | shipping, hardware-verified |
| `6a40f006` | Diagnostics | Read/Notify | built, hardware-verified |
| `6a40f007` | Command | Write | built, **never run on hardware** — 7 ops, physical ones refused while vending |
| `f008`–`f00d` | OTA, WiFi | — | not built |

Full byte layouts and the complete UUID allocation are in
`docs/BLE_CONFIG_CONTRACT.md`.

> ⚠️ **Device Info is deliberately not in the default build.** The mobile app
> decides a board's whole profile on whether `f001` exists: present means "full"
> and triggers a sync chain that also needs `f004`, `f006` and `f007`; absent
> means "config only", which is why Config works today. **Every characteristic
> that chain needs now exists** (`f002`, `f003`, `f004`, `f006`, `f007`). What
> still holds the flag off is app-side: a board reporting a real serial breaks
> every machine claimed before Device Info existed, which needs a re-claim path
> in the app first (`A7` in `docs/APP_BLE_PLAN.md`).
>
> Build `-e esp32dev-devinfo` to expose it for nRF Connect bench testing, which
> has no notion of app profiles. Use the default `-e esp32dev` for any board the
> app will touch.

## Design notes

**The watchdog is external, unmaskable, and fast.** The TPL5010 (U5) drives
`ESP_EN`, so an unpetted watchdog hard-resets the module and no software can stop
it. Its window may be as short as ~100 ms — versus the STM32's 25 s internal
IWDG — so petting lives in a dedicated highest-priority task rather than being
sprinkled through application code. **A blocking call anywhere in the vend path
does not merely freeze that path, it reboots the board mid-vend, and the fault
then looks like a hardware problem.** Read `src/wdt.h` before adding anything
that blocks, and note `PENDING.md` item 1: the app-level supervision the STM32
had is scaffolded but not yet switched on.

**All I2C is confined to the display task.** Callers post to a queue and never
block. A wedged LCD costs you the screen and nothing else — the relay timer and
session countdown are unaffected. This mattered on the STM32; with a ~100 ms
watchdog it matters much more.

**Output pins come up safe.** Every MOSFET gate is written LOW *before*
`pinMode(OUTPUT)`, so the pad's output register already holds 0 the moment the
driver is enabled. Three of our pins (IO2, IO12, IO15) are also **strapping
pins** sampled at reset — IO12 high at reset can force 1.8V flash timing and stop
the board booting — so `periph_reset_safe()` safes them before any software
reset. External pull-downs cover a cold power-on.

**Config applies at IDLE.** A menu save is persisted immediately, but the live
values the state machine reads are refreshed on return to idle, so relay time and
mode cannot change under a paying customer.
