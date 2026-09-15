# Porting the VendoBoard firmware from STM32F103C8 to ESP32 (Vendo S1)

Source of truth for the port: `c:\vendo\Vendo_Production\VendoBoard_ProductionFirmware`,
branch **`main`**, commit **`ee8e253`** ("added software version info").

That tree already embodies every commit on `main` — including all the bug fixes
listed at the bottom of this document — so porting it faithfully carries the
whole history forward rather than replaying it. The reasoning behind those fixes
was read out of `BUGS_HANDOVER.md` and the commit log and is preserved in comments
at the places it matters, so the same mistakes are not re-introduced later.

The daisy-chain master/slave work on branch `ckabiling/slave_non_RTOS` is
**deliberately not in this pass** — see `PENDING.md` item 6.

---

## 1. File-by-file correspondence

The structure is deliberately kept parallel so the two codebases stay diffable.

| STM32 | S1 | Change |
|---|---|---|
| `include/pins.h` | `include/pins.h` | Rewritten for the S1 board; names kept identical |
| `include/version.h` | `include/version.h` | `1.0.0` → `1.0.0-esp32` |
| `include/debug_log.h` | `include/debug_log.h` | `Serial1` → `Serial` |
| `include/STM32FreeRTOSConfig.h` | — | Dropped; FreeRTOS config comes from the ESP-IDF sdkconfig |
| `src/main.cpp` | `src/main.cpp` | No `vTaskStartScheduler()`; byte stack sizes; core pinning; CLI moved into `loop()` |
| `src/config.h/.cpp` | `src/config.h/.cpp` | EEPROM → NVS; one appended field; new magic |
| `src/periph.h/.cpp` | `src/periph.h/.cpp` | LEDC buzzer; polarity-aware coin input; remapped buttons; watchdog split out |
| — | `src/wdt.h/.cpp` | **New** — external TPL5010 watchdog |
| `src/session.h/.cpp` | `src/session.h/.cpp` | Near-verbatim; `watchdog_feed` is now a liveness check-in |
| `src/app.h/.cpp` | `src/app.h/.cpp` | Near-verbatim; config-menu gestures remapped |
| `src/display.h/.cpp` | `src/display.h/.cpp` | Same API and same screens; I2C recovery re-expressed |
| `lib/LiquidCrystal_I2C/` | `src/lcd.h/.cpp` | Replaced with a driver that reports ACK/NAK |
| `src/cli.h/.cpp` | `src/cli.h/.cpp` | All commands ported + ESP32-only ones (`AT+WDT?`, `AT+STRAP?`, `AT+LCD?`, `AT+COIN?`, `AT+SERIAL*`, `AT+RTC`, `AT+COUNTERS?`) |
| — | `src/identity.h/.cpp` | **New** — factory serial, own NVS namespace |
| — | `src/rtc.h/.cpp` | **New** — SLM1302 RTC + UTC time service |
| — | `src/counters.h/.cpp` | **New** — earnings totals, own NVS namespace |
| — | `src/ble_config.*`, `ble_timesync.*`, `ble_livecounters.*`, `ble_deviceinfo.*` | **New** — BLE GATT server and characteristics |
| `lib/Wire/` (vendored) | — | Dropped; the vendored copy existed for an STM32-specific buffer fix |

## 2. The six changes that are behaviour, not mechanics

Everything else is a mechanical translation. These six are worth knowing about.

### 2.1 Watchdog: internal 25 s IWDG → external ~100 ms TPL5010

The single biggest change, and the one with a **pending item attached**
(`PENDING.md` items 1–2). Full rationale is in `src/wdt.h`.

The STM32 had an internal IWDG at ~25 s, kicked by `watchdog_feed()` calls inside
the app/session/menu loops. The S1 has a **TPL5010 outside the ESP32** whose
`nRST` drives the `ESP_EN` net: nothing in software can mask it, the timeout is
fixed by R34 in hardware, and it is short — the part's range bottoms out near
100 ms, which is the interval requested for now.

A 100 ms budget cannot be met by feed calls in application code, because the app
legitimately blocks far longer than that in places the 25 s IWDG never minded:
the 2 s boot splash, the 3 s "COMPLETE!" hold, the 600 ms denied message, a
multi-beep pattern. So:

- petting moved to a **dedicated highest-priority task** that pulses DONE every
  100 ms via `vTaskDelayUntil` (no cadence drift);
- `watchdog_feed()` kept its STM32 name and every call site, but became a
  **liveness check-in** that stamps a per-source timestamp;
- **consequence, deliberate and pending:** the watchdog currently catches total
  system death but *not* an app-task hang, which the STM32 did catch. The
  scaffolding to restore it is in place and `AT+WDT?` exposes it; enforcement is
  one `if`, held back until the real timeout is measured. See `PENDING.md`.

`IO2` is also a strapping pin, so DONE is driven low before `pinMode(OUTPUT)`
and never left high — a pulse that coincided with a reset would otherwise pick
an unintended boot mode.

### 2.2 Config menu: three buttons → two

The STM32 had BTN1/BTN2/BTN3 (PB13/14/15). The S1 breaks out only **SW2 (IO34)**
and **SW3 (IO35)**, both on input-only pads. BTN3's three jobs were redistributed
onto the combo and the START button:

| Action | STM32 | Vendo S1 |
|---|---|---|
| Enter config (from IDLE) | BTN3 long press | **BTN1+BTN2 long press** |
| Adjust value | BTN1 / BTN2 | BTN1 / BTN2 *(unchanged)* |
| Next page | BTN3 short press | **START tap** |
| Save all pages | BTN3 long press | **BTN1+BTN2 long press** |
| Cancel, discard | BTN1+BTN2 long press | **START long press** |

Three details make this safe rather than merely compact:

- **START carries three meanings from one state machine.** Press-confirm (start a
  session, IDLE only), short-on-release (next page), long-while-held (cancel).
  One machine means one physical press can only ever satisfy one of them — so
  cancelling cannot also turn a page on the way out. `start_button_flush()` is
  called on menu entry and exit so a press cannot leak across the boundary.
- **There is no ambiguity with starting a session.** Entering the menu needs the
  BTN1+BTN2 combo, so a START tap can never mean both things at once.
- **The combo cannot double-fire.** After firing it waits for both buttons to be
  released, so the hold that opens the menu cannot instantly also save it.

**A bug this remap introduced, and its fix.** On the STM32 the combo meant
*cancel*, so the fact that holding both buttons also scrubs the current page's
value was harmless — the changes were discarded anyway. Here the combo means
**save**, so anything the hold scrubbed would be committed. Worse, it was not
even symmetric: on the boolean pages (Per Credit, Accumulation, Sensor Stop,
Sensor Polarity) BTN1 writes `false` and BTN2 writes `true` in the same
iteration, BTN2 second — so simply holding both to save would have silently
forced those settings **ON**. `btn_b1_pressed()`/`btn_b2_pressed()` now suppress
their result while the other button is down. The state machines are still polled,
so debounce and hold timing stay coherent and releasing one button resumes
scrubbing with the other.

### 2.3 Storage: flash-emulated EEPROM → NVS

`EEPROM.get/put(0, AppConfig)` became one NVS blob under `vendo/appcfg` via
`Preferences`. Strictly simpler: on STM32F1 every emulated-EEPROM byte write
erased and reprogrammed a 1 KB page (which is why the sibling slave firmware had
to hand-roll a buffered write path), whereas NVS batches and wear-levels. The
magic check still earns its keep — a short or absent blob and a layout change
both land as "stale" and fall back to defaults exactly as before.

`AT+EEPROM?` and `AT+EEPROM_RESET` keep their STM32 names so muscle memory and
any existing test scripts still work.

### 2.4 `AppConfig` gained one field, so the magic diverged

`APP_CONFIG_MAGIC` went `0xBEEFCB07` → **`0xBEEF5107`** because the struct has one
extra trailing field. Keeping the STM32 value would have let an S1 board accept a
byte pattern it cannot correctly parse.

The new field is **`coin_active_high`**, and it exists because this is money. The
STM32 hard-coded "idle LOW, pulse HIGH"; on the S1 the pulse crosses the PC817
opto and the schematic does not say which way its output swings. Rather than bake
a guess into the build, it is a settable field (`AT+COIN_POLARITY`, applied live
so bench calibration is a fast loop).

**The default changed on 2026-09-14** — it was `false` (idle HIGH, pulse LOW), a
pure guess; it is now **`true` (idle LOW, pulse HIGH)**, derived from the S1
schematic's COIN_SLOT sheet. At idle the opto LED is driven through R22/R24 so the
phototransistor conducts and COIN_IN sits LOW; a coin pulse sinks the acceptor
output, the LED goes dark, and R25 pulls COIN_IN to +3.3 V. The fail-safe
direction agrees: an unplugged acceptor floats to +5 V, LED on, COIN_IN LOW —
i.e. idle, rather than a permanently asserted coin line.

Coins have since been counted on a real board with this default, but it is **not
yet confirmed with a meter** — `PENDING.md` item 3 stays open.

### 2.5a S1-only additions with no upstream counterpart

**Intended divergence, listed here because the diffability rule says anything not
enumerated in this section should match the STM32.** None of this is a port; the
STM32 has nothing to diff it against, and a change on either side does *not*
belong on both.

| Module | What it is |
|---|---|
| `src/ble_config.cpp` | BLE GATT server, gated behind `-DENABLE_BLE`. Owns the service and the 37-byte Config characteristic (`6a40f005`). Other characteristics register themselves into its service. |
| `src/ble_timesync.cpp` | Time Sync (`6a40f002`), write `epoch_utc:u32`. |
| `src/ble_livecounters.cpp` | Live Counters (`6a40f003`), read + notify, 14 bytes. |
| `src/ble_deviceinfo.cpp` | Device Info (`6a40f001`). **Built but OFF by default** — see the warning below. |
| `src/identity.h/.cpp` | Factory serial `VLABS-S1-NNNNN`, write-once, own NVS namespace. The app's `device_id`. |
| `src/rtc.h/.cpp` | SLM1302 3-wire RTC driver + UTC time service. Ported from the bring-up harness, not from the STM32. |
| `src/counters.h/.cpp` | Earnings totals behind Live Counters. Own NVS namespace. |
| `src/ble_diagnostics.cpp` | Diagnostics (`6a40f006`), read + notify, fault mailbox. |
| `src/ble_command.cpp` | Command (`6a40f007`), write. A mailbox drained from `loop()` — never executes in the GATT callback. Physical ops refused unless `app_state_is_idle()`. |
| `src/ble_sessionlog.cpp` | Session Log (`6a40f004`), stateful write-cursor-then-read pagination. Cursor is per BLE connection. |
| `src/eventlog.h/.cpp` | The event store behind it — **one row per paid period**, hooked at `coin_consume_value_cents()` (billing) and `APP_STATE_SESSION_END` (close). **Its own 128 KB flash partition** (`vendolog`, `partitions_vendo.csv`), not NVS — a ring of 8,192 rows wrapping oldest-first. |

Three things in here are load-bearing and easy to undo by accident:

**1. Identity and counters are NOT in `AppConfig`, on purpose.** Both have their
own NVS namespaces, untouched by `config.cpp`. `config_load()` falls back to
`config_defaults()` on a magic mismatch, and §2.4 requires bumping
`APP_CONFIG_MAGIC` on any layout change — so a serial or a lifetime total living
in that blob would be silently reassigned or zeroed as a *side effect of the
guard working correctly*. A factory reset must clear settings, not identity or
takings.

> **Standing rule:** *identity and monotonic counters are not configuration.*
> The event log obeys it too: its records live in the `vendolog` flash partition
> and its `seq` floor lives in `vendocnt`, so neither a magic bump nor a factory
> reset can rewind sequence numbers into ids the backend has already filed.

A useful consequence: none of this needed a magic bump, so §2.4's divergence
still stands alone and the hardware-verified 37-byte Config frame is untouched.

**2. Device Info (`f001`) must not ship alone.** The mobile app decides a board's
entire profile on whether that characteristic exists — present means `full` and
triggers a sync chain that requires `f004` and `f006` too; absent means
`configOnly`, which is why Config works today. Exposing it early makes the app's
connect fail outright, taking the working Config push with it. Hence the separate
`[env:esp32dev-devinfo]` PlatformIO environment rather than a flag in the default
build. See `src/ble_deviceinfo.h`.

**3. Trickle charging is disabled in `rtc_init()` every boot, and read back.**
BT1 is a CR2032 — non-rechargeable. Charging it can vent the cell, so this is a
safety property, not a setting. Enabling it requires two deliberate macros and is
a compile error otherwise.

Still unbuilt: OTA (`f008`–`f00a`, `f00d`) and WiFi (`f00b`–`f00c`). Everything
the app's sync sequence needs — `f001`–`f007` — exists as of 2026-09-15. See
`docs/APP_BLE_PLAN.md`.

**One STM32-diffability note from `f007`:** `app.cpp` gained a single line,
`app_publish_state(state)` at the top of the state machine loop, so other tasks
can ask whether the board is vending. It is the only outward-facing state the
app task exposes, and it exists because a BLE relay test during a paid session is
a customer complaint. Nothing reads it from inside `app.cpp`.

### 2.5 I2C recovery: register pokes → driver-reported status

The failure modes are identical (stuck slave holding SDA, latched peripheral,
error flags) and the *policy* is unchanged — a fast ~3 ms unlock during a session
so the relay/EMI window is not extended, a full re-init only at IDLE, LCD
disabled after 3 consecutive failures. Only the mechanics changed:

| Detection | STM32 | Vendo S1 |
|---|---|---|
| Stuck slave | `digitalRead(SDA) == LOW` | same *(`PENDING.md` item 4)* |
| Error flags | read `I2C1->SR1` BERR/ARLO/AF | `lcd_write_failures()` — actual NAK count |
| Busy latch | `I2C_CR1.SWRST` | `Wire.end()` + `Wire.begin()` |
| Bus unlock | 9 SCL bit-bang pulses + STOP | same |

Reading NAKs from the driver is better information than the STM32 had: it reports
what the peripheral actually did rather than what the bus looks like. Getting it
required replacing `LiquidCrystal_I2C`, which swallows every transaction result —
hence `src/lcd.h/.cpp`, which is the same hand-rolled PCF8574 driver already
proven on this exact board by the bring-up harness.

## 3. Smaller mechanical differences

- **FreeRTOS is already running** when `setup()` is entered, so there is no
  `vTaskStartScheduler()`. Tasks start the moment they are created, which makes
  creation *order* matter: the wdt task is created first, before any boot
  printing (`config_dump_eeprom()` alone is several hundred bytes at 115200 —
  well past 100 ms).
- **Stack sizes are bytes, not words.** Copying the STM32 numbers (256/512/768)
  verbatim would have given a quarter of the stack and overflowed on the first
  `snprintf`.
- **Tasks are pinned to core 1**, leaving core 0 for the WiFi/BLE stacks this
  board is eventually meant to use. Pinning them together also means a task that
  spins without yielding starves core-1 idle and trips the ESP32's own task
  watchdog — free supervision that spreading them would lose.
- **The CLI moved from its own task into `loop()`.** `loopTask` already exists
  with an 8 KB stack and polling a UART every 10 ms is exactly what it is for.
  Same behaviour, one less task.
- **Buzzer: `tone()` → LEDC.** ESP32 Arduino has no `tone()`. Driving a square
  wave is also the safer bet on unknown hardware: BZ1 may be a passive sounder,
  which is silent on a DC gate, whereas an active buzzer still sounds (chopped)
  on a square wave. The 2.x/3.x LEDC API split is handled the same way the
  bring-up harness handles it.
- **Buzzer no longer kicks the watchdog per beep.** It did on the STM32, where a
  long pattern could approach the 25 s window. Against a ~100 ms window that
  could never have worked — a single beep is longer than the window — so the wdt
  task owns it.
- **One debug LED, not two.** `leds_set()` drove PA2+PA3 in lockstep; the S1 has
  only D7 on IO15. Call sites unchanged.
- **`pinMode` is `INPUT`, not `INPUT_PULLUP`, for the buttons.** Every button here
  has its own external pull-up, and IO34/IO35 have no internal pull-up available
  at all — so this is forced as well as correct.
- **Strapping pins need safing before a software reset.** `periph_reset_safe()`
  is new: IO12 high at reset can force 1.8V flash timing and stop the board
  booting, IO2 high picks an unintended boot mode, IO15 gates the ROM boot log.
  External resistors cover a cold power-on; only firmware covers a warm restart.
  The STM32 needed no equivalent — its GPIOs all revert to inputs and nothing was
  sampled at the reset instant.
- **Safety hooks.** `vApplicationStackOverflowHook` /
  `vApplicationMallocFailedHook` are strong symbols in ESP-IDF and cannot be
  overridden; those conditions panic and reboot on their own. The relay is still
  safe by the same mechanism the STM32 relied on *after* its reset: every ESP32
  GPIO reverts to a high-impedance input and Q2's gate has R19 10K to ground, so
  the coil de-energises with no firmware involvement. An
  `esp_register_shutdown_handler` covers the clean restart paths slightly earlier.
- **`static_assert(configTICK_RATE_HZ == 1000)`** in `session.cpp`. The STM32
  relied on 1 tick == 1 ms for its tick arithmetic (notably `SESSION_TICKS_CAP`,
  written as a direct multiplication precisely because `pdMS_TO_TICKS` of a 7-day
  value overflows mid-macro). Arduino-ESP32 also runs at 1000 Hz — but the port
  asserts it rather than inheriting it silently, so a future core change breaks
  the build instead of quietly mis-timing a vend.
- **`config_load_quiet()`** is new. The CLI setters need to read stored config
  without `config_load()`'s two boot-path side effects: printing the whole config
  (which would bury the command's response) and persisting defaults on a miss
  (which would turn a read-only query into a flash write).

## 4. Pin mapping

Names are identical across both codebases; only the pin differs.

| Function | STM32 | Vendo S1 | Note |
|---|---|---|---|
| Relay | PA6 | **IO19** | active-HIGH gate, same polarity |
| Buzzer | PA1 | **IO27** | LEDC, not `tone()` |
| Debug LED | PA2 + PA3 | **IO15** | one instead of two; also a strap (MTDO) |
| User LED | PB4 | **IO23** | sinks the J1 button LED |
| START button | PB3 | **IO18** | also absorbed BTN3's duties |
| Config BTN1 | PB13 | **IO34** | input-only pad |
| Config BTN2 | PB14 | **IO35** | input-only pad |
| Config BTN3 | PB15 | **— none** | gestures remapped, §2.2 |
| Coin pulse in | PA5 | **IO17** | via PC817 opto; polarity now configurable |
| Coin inhibit | PA4 | **IO12** | active-HIGH, same polarity; **strap** |
| Sensor Stop in | PB0 | **IO32** | J6 AUX_1; no dedicated pin exists |
| Role strap | PA8 | **IO5** | JP10; reported, not acted on |
| I2C SDA / SCL | PB7 / PB6 | **IO21 / IO22** | |
| Watchdog DONE | — | **IO2** | new; **strap** |
| Dispense INT | — | **IO16** | reserved, `PENDING.md` item 6 |
| Log UART | PA9/PA10 (Serial1) | **IO1/IO3 (Serial)** | J7 PROG header |

## 5. What is verified, and what is not

**Verified:** the firmware compiles clean with zero warnings against
`espressif32@^6.9.0` (24.1% flash, 6.7% RAM), and the `configTICK_RATE_HZ`
assertion holds, which is what makes the ported session timing sound.

**Not verified:** nothing has run on hardware. Every behavioural claim here is a
faithful port of logic that worked on the STM32, not an observation of this
binary. The bench items are `PENDING.md` 2–5, and the coin polarity (item 3) is
the one that touches money.

## 6. Bug fixes inherited from `main`'s history

These were all fixed on the STM32 before `ee8e253` and are therefore present in
the port. Listed so nobody "simplifies" one of them back out — the reasoning is
also in comments at each site.

- **Start-button auto-repeat caused relay chatter in Pause/Resume** (`8e0c203`).
  `start_button_pressed()` shared its debounce helper with the config-menu
  scrubbing buttons, which auto-repeat after ~500 ms and accelerate to ~40 ms. In
  `OP_PAUSE_RESUME` every repeat flipped the relay, so holding the button
  chattered it for as long as it was held. The `repeat` parameter in
  `btn_simple_poll()` exists solely to keep those two behaviours apart.
- **Accumulation bypassed per-credit gating.** A coin dropped mid-block silently
  stretched the running block, defeating the one-press-per-credit gate. Now
  queued as extra gated credits via `extra_credits_out`.
- **Accumulation missed coins landing right at session expiry** (`c84f1df`).
  `session_apply_accumulation()` is called twice per iteration — before *and*
  after `vTaskDelay` — so a coin landing during that sleep is applied before the
  loop's exit condition is re-checked.
- **Unbounded session extension could wrap the countdown to zero** (`c84f1df`).
  `SESSION_TICKS_CAP`, written as a direct multiplication because `pdMS_TO_TICKS`
  overflows on a 7-day value.
- **Accumulation did not bill until the next block started** (`fae7de3`). Coins
  dropped on the "press for next credit" screen now bill immediately and refresh
  the LCD, instead of waiting for the next block.
- **User LED stayed off with credit remaining in per-credit mode** (`a7e5d50`).
  `APP_STATE_WAIT_NEXT_CREDIT` drives it HIGH while `remaining_credits > 0`.
- **Auto Dispense billed only part of a deposit** (`5810e6b`). Auto Start now
  waits out a 500 ms settle window with no coin activity before firing, so a
  whole deposit is billed together — without it, two coins dropped back-to-back
  billed as one credit.
- **Credit carryover discarded leftover coins** (`1410a29`). Only the billed
  amount is consumed; leftover cents stay banked for the next credit or session.
- **Config-menu cancel was false-triggered** (`4593877`, `07ab779`). The gesture
  moved off a BTN3 double-tap entirely. On the S1 it is START long press (§2.2).
- **Coin slot force-disabled inside the config menu** (`50c4939`), regardless of
  mode or any other setting.
