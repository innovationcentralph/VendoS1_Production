# Vendo_S1_ProductionFirmware

Vending control firmware for the **ESP32-WROOM-32 Vendo S1 board** — coin
billing, session/relay timing, three operation modes, 16x2 LCD config menu, AT+
serial CLI. Arduino framework on `espressif32@^6.9.0`, FreeRTOS tasks (the core
brings its own; there is no `lib_deps`).

This is a **port**, not an original. Read `docs/PORTING_FROM_STM32.md` before any
non-trivial change — it is the map of what diverged from the upstream and why.

> **Nothing in this repo has run on hardware** (as of 2026-08-24). It compiles
> clean, and the logic is a faithful port of firmware that works in the field, but
> every behavioural claim here is "faithful port", not "observed". Do not describe
> anything as verified-on-hardware without having actually run it.

## Upstream, and the diffability rule

Ported from `c:\vendo\Vendo_Production\VendoBoard_ProductionFirmware`, branch
**`main`** @ **`ee8e253`** — a separate git repo (STM32F103C8, FreeRTOS, same
product, same vend behaviour).

**File names, function names, struct fields and screen text were kept identical
on purpose** so the two trees stay diffable. `PIN_RELAY` means the same thing on
both boards even though one is PA6 and the other IO19; `AT+EEPROM?` kept its name
even though storage here is NVS.

So: **a vend-behaviour change usually belongs on both sides.** Before "fixing"
something in `app.cpp`, `session.cpp` or `display.cpp`, check whether the STM32
has the same code — if it does, the fix probably belongs there too, and silently
diverging is how the two firmwares start behaving differently in the field. The
places where divergence is *intended* are enumerated in
`docs/PORTING_FROM_STM32.md` §2 and §3; anything not on that list should match.

## Workspace layout

Five separate git repos, opened side by side. Not a monorepo — don't assume
context in one is visible from another except through what is written down.

```
Vendo_S1_ProductionFirmware/       <- this repo (ESP32 product firmware)
VendoBoard_ProductionFirmware/     <- the STM32 upstream this was ported from
Vendo_S1_TestCode/                 <- bring-up harness for THIS board
Vendo_I2C_Master_Slave/            <- STM32 bare-metal I2C rig + the hardware docs
Vendo_C1_TestCode/                 <- harness for the STM32 C1 board
```

- **Hardware source of truth:** `../Vendo_I2C_Master_Slave/docs/hardware/ESP32_Vendo_Board.md`.
  If it and `include/pins.h` disagree, the schematic wins and `pins.h` is the bug.
- **`../VendoLabs_TestCodes/Vendo_S1_TestCode`** proves the board section by section from a CLI. It
  already has the rigs for the measurements `docs/PENDING.md` asks for (watchdog
  timeout via `AT+WDT=0`, `I2C_INT` rise via `AT+INT=M`). Prove a board there
  before blaming this firmware.

## Build / flash

```
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COMx
pio device monitor -e esp32dev
```

`pio` is not on PATH in this environment; it lives at
`~/.platformio/penv/Scripts/pio.exe`.

Flashing is **manual** — J7 (PROG) has no RTS/DTR auto-reset network: hold BOOT
(SW7), tap RESET (SW1), release BOOT, upload, tap RESET again. Same shape as the
STM32 rig's BOOT0 procedure.

`build_flags`: `ENABLE_CLI` on by default (drop for production — it is the only
way to set beep tones, display type, coin polarity or factory-reset without
reflashing), `VERBOSE_LOG` commented out (chatty per-coin/per-tick traces).

## The watchdog is the thing to be careful about

**`src/wdt.h` is required reading before adding anything that blocks.**

The STM32 had an internal IWDG at ~25 s. This board has a **TPL5010 outside the
ESP32** whose `nRST` drives the `ESP_EN` net: unmaskable by software, timeout
fixed in hardware by R34, and **possibly as short as ~100 ms**.

Consequences that change how code gets written here:

- **A blocking call in the vend path does not freeze that path — it reboots the
  board mid-vend, and the fault then looks like a hardware problem.** This is the
  single biggest behavioural hazard in the repo. Anything that waits must be a
  timed state or must `vTaskDelay`.
- **Petting lives in one dedicated highest-priority task** (`wdt_task_run`), on
  `vTaskDelayUntil` so the cadence cannot drift. Do not move petting back into
  application code — the app legitimately blocks for 2 s (boot splash) and 3 s
  ("COMPLETE!" hold), which a ~100 ms window can never absorb.
- **Do not add `wdt_pet()` calls anywhere.** It races the wdt task on IO2 and can
  cut a DONE pulse below the TPL5010's minimum width — which is a reset, the exact
  opposite of the intent. There is a comment saying so in `lcd_autodetect()`,
  where the bring-up harness's version *did* pet and this one deliberately does
  not.
- **`watchdog_feed()` no longer pets.** It kept its STM32 name and every STM32
  call site, but records a per-source liveness timestamp instead.

**Known gap, deliberate, `docs/PENDING.md` item 1:** because petting is
unconditional, an *application* hang is not caught, which the STM32 did catch.
Enforcement is one `if` at a marked spot in `wdt_task_run()`, held back until the
real timeout is measured (item 2). Don't switch it on without a measured number —
a deadline guessed too tight reboots mid-vend, which is worse than the gap.

## Task layout

FreeRTOS is **already running** when `setup()` is entered — there is no
`vTaskStartScheduler()` to port, and tasks start the instant they are created.

- **Creation order matters.** The wdt task is created *first*, before any boot
  printing; `config_dump_eeprom()` alone is several hundred bytes at 115200, well
  past 100 ms.
- **Stack sizes are BYTES, not words.** The STM32's 256/512/768 were words —
  copying them verbatim gives a quarter of the stack and overflows on the first
  `snprintf`.
- **All tasks are pinned to core 1**, leaving core 0 for the WiFi/BLE stacks this
  board is eventually meant for. Keeping them together also means a task that
  spins without yielding starves core-1 idle and trips the ESP32's own task
  watchdog — free supervision that spreading them would lose.
- **The coin task runs above the app task.** It counts money; that ordering is
  from the STM32 and should stay.
- **The CLI lives in `loop()`**, not its own task (the STM32 had `TaskCli`).
  `loopTask` already exists with an 8 KB stack and polling a UART at 10 ms is what
  it is for.

## Config menu gestures — two buttons, not three

The S1 breaks out only SW2/SW3, so the STM32's BTN3 duties were redistributed
(user's decision, 2026-08-24; they declined using SW7/BOOT as a third button):

| Action | STM32 | here |
| --- | --- | --- |
| Enter config (from IDLE) | BTN3 long | **BTN1+BTN2 long** |
| Adjust value | BTN1 / BTN2 | unchanged |
| Next page | BTN3 short | **START tap** |
| Save | BTN3 long | **BTN1+BTN2 long** |
| Cancel | BTN1+BTN2 long | **START long** |

Three invariants hold this together. Breaking any of them is a user-facing bug:

- **`btn_b1_pressed()`/`btn_b2_pressed()` suppress their result while the other
  button is down.** Not cosmetic. The combo used to mean *cancel* (so scrubbing
  was discarded); it now means *save*, and BTN1 writes `false` / BTN2 writes
  `true` in the same iteration with BTN2 second — so without suppression, holding
  both to save silently forces the boolean pages (Per Credit, Accumulation,
  Sensor Stop, Sensor Polarity) **ON**.
- **START's three meanings come from ONE state machine** — press-confirm (start a
  session), short-on-release (next page), long-while-held (cancel). One machine is
  what guarantees a single press satisfies only one of them, so cancelling cannot
  also turn a page on the way out. Don't split it. `start_button_flush()` on menu
  entry/exit stops a press leaking across the boundary.
- **`btn_b1_b2_long_pressed()` waits for both buttons to be released before it can
  re-arm**, which is why the hold that opens the menu cannot instantly also save
  it. Don't reset that state machine on menu entry.

## Money invariants — don't "simplify" these

Billing is **money-first**. `coins_required` is the *peso price of one credit*
(`* 100` = centavos), **not** a pulse count — the name is misleading and is kept
only for STM32 parity.

```
coins inserted = raw pulses x price_per_credit_cents
credits        = floor(coins inserted / (coins_required x 100))
relay ON time  = credits x relay_on_ms
```

- **Leftover cents are never discarded.** Only the billed amount is consumed;
  remainders stay banked for the next credit or session. Never reset the money
  total as a shortcut.
- **Coin pulse polarity is a config field**, not a constant
  (`AppConfig::coin_active_high`, `AT+COIN_POLARITY`, applied live). The PC817
  opto's output swing is not in the schematic and this is the coin input — getting
  it wrong means taking money and counting nothing. `docs/PENDING.md` item 3;
  confirm on hardware before changing the default.
- **Accumulation must not stretch a block that is running under per-credit
  gating** — mid-block coins queue as extra gated credits via
  `extra_credits_out`. Collapsing that back into a deadline extension defeats the
  one-press-per-credit gate.
- **`session_apply_accumulation()` is called twice per loop iteration**, before
  *and* after `vTaskDelay`, so a coin landing during that sleep is applied before
  the loop's exit condition is re-checked. Removing the second call loses coins at
  session expiry. (STM32 fix `c84f1df`.)
- **`SESSION_TICKS_CAP` is written as a direct multiplication, not
  `pdMS_TO_TICKS`** — that macro overflows mid-computation on a 7-day value.
  `session.cpp` has a `static_assert(configTICK_RATE_HZ == 1000)` guarding the
  1-tick-equals-1-ms assumption the arithmetic rests on; if it ever fires, fix the
  arithmetic rather than deleting the assert.
- **Auto Start waits a 500 ms settle window** with no coin activity before firing,
  so a whole deposit bills together. Without it two coins dropped back-to-back
  bill as one credit. (STM32 fix `5810e6b`.)

## Display

- **All Wire/LCD access is confined to `display_task_run()`.** Callers post to a
  queue and never block. Wire is not re-entrant, and per the watchdog section a
  blocking I2C call on the app task is a reboot, not a stall.
- **`src/lcd.h/.cpp` replaced `LiquidCrystal_I2C` on purpose.** That library
  swallows every transaction result; the STM32 compensated by reading `I2C1->SR1`
  status registers, which the ESP32 does not expose. This driver reports the real
  ACK/NAK count (`lcd_write_failures()`), which is what `i2c_needs_recovery()`
  uses. Don't swap a library back in — it would take the recovery detection with
  it. The PCF8574 bit mapping at the top of `lcd.cpp` is a *convention*, not a
  standard: if the display lights up and shows garbage, change that block first.
- **Recovery policy** (unchanged from the STM32): fast ~3 ms bus unlock during a
  session so the relay/EMI window isn't extended, full re-init only at IDLE, LCD
  disabled after 3 consecutive failures, 5 s no-message heartbeat re-init to heal
  garble that didn't trip an error.

## Storage

One NVS blob (`vendo`/`appcfg`) via `Preferences`, replacing the STM32's
flash-emulated EEPROM. No page-erase-per-byte problem, so no buffered-write
trick is needed.

**Three things deliberately live OUTSIDE that blob, in their own namespaces:**

| Namespace | Holds | Why not `AppConfig` |
| --- | --- | --- |
| `vendoid` | the factory serial (`src/identity.*`) | a magic bump or factory reset must never reassign a board's identity |
| `vendocnt` | earnings totals **and the event log's `seq` floor** (`src/counters.*`) | same, for lifetime takings; and rewinding `seq` corrupts backend delta sync |

**Standing rule: identity and monotonic counters are not configuration.**
`config_load()` falls back to `config_defaults()` on a magic mismatch, so
anything in the blob gets silently reset *as a side effect of the guard working
correctly*. That is right for settings and catastrophic for identity. Nothing in
`config.cpp` reads or writes these namespaces.

- **Bump `APP_CONFIG_MAGIC` on any `AppConfig` layout change.** A short or absent
  blob and a stale magic both fall back to defaults, which is the guard working.
- **The magic deliberately differs from the STM32's** (`0xBEEF5107` vs
  `0xBEEFCB07`) because this struct has one extra trailing field. Don't "restore"
  parity — it would let a board accept bytes it cannot parse.
- **`config_load()` is the boot path; `config_load_quiet()` is for the CLI.** The
  former prints the whole config and persists defaults on a miss — both wrong for
  a read-only query.
- CLI setters edit the *stored* copy and say `NOTE: send AT+RESET to apply`. The
  LCD menu is the path that applies immediately. `AT+COIN_POLARITY` is the one
  exception (applied live, because bench calibration needs a fast loop).

**The event log is not in NVS at all — it has its own flash partition.**
`vendolog`, 128 KB, 8,192 records of 16 bytes, defined in
`partitions_vendo.csv` and driven by `src/eventlog.*`. Two rules come with it:

- **Nothing below `0x290000` in that CSV may move.** `nvs` at `0x9000` holds the
  write-once factory serial, and `app0`/`app1` offsets are baked into the OTA
  data and into every binary already flashed. The 128 KB was taken from the
  `spiffs` partition, which nothing in this firmware mounts.
- **A board flashed from an older build keeps its old partition table** until
  `partitions.bin` is uploaded (a normal `-t upload` does this). Until then the
  boot banner says `` `vendolog` partition not found — Session Log (f004)
  DISABLED`` and no events are recorded. Nothing else is affected.

## BLE — and the one trap in it

NimBLE GATT server behind `-DENABLE_BLE`. `ble_config.cpp` owns the service and
Config (`f005`); every other characteristic is its own file registering into that
service — `ble_timesync.cpp` (`f002`), `ble_livecounters.cpp` (`f003`),
`ble_sessionlog.cpp` (`f004`), `ble_deviceinfo.cpp` (`f001`),
`ble_diagnostics.cpp` (`f006`), `ble_command.cpp` (`f007`). Keep that shape:
OTA and WiFi (`f008`–`f00d`) are still to come.

**Session Log (`f004`) is the only stateful one.** The app writes a 4-byte
`after_seq` cursor, then reads a page, then repeats with **the last seq it
received** — so the cursor is exclusive on *every* page, not just the first, and
the board must filter `seq > cursor` each time. The cursor is kept **per
connection** (keyed on the NimBLE connection handle, released in the server's
`onDisconnect`): the ESP32 permits three connections, and one global cursor
would let two phones corrupt each other's pagination.

UUIDs come from the app team's allocation table in `docs/BLE_CONFIG_CONTRACT.md`.
**Never mint one by guessing the next free value** — the app has them compiled in,
and a mismatch is a silent misparse, not an error.

**`f001` Device Info is a capability probe, not just data.** The app decides a
board's entire profile on whether it exists: present means `full` and triggers a
sync chain needing `f004` and `f006` too; absent means `configOnly`, which is why
Config works today. Exposing it early makes the app's connect fail outright,
**taking the working Config push with it**. It is therefore built but OFF —
`pio run -e esp32dev-devinfo` for nRF Connect bench work, default env for
anything the app will touch. **Every characteristic that gate was waiting on now
exists** (`f002`, `f003`, `f004`, `f006`, `f007`, 2026-09-15). The flag stays out
of `[env:esp32dev]` for one reason that is **not firmware's to fix**: the app
asserts `deviceInfo.deviceId === machine.deviceId` on every `full` connect, and
machines claimed before `f001` existed stored the BLE MAC there. The first board
to report a real serial breaks every already-claimed machine in that install.
Flip it when the app has that re-claim path (`A7`), not before.

**`f007` Command has one rule worth knowing before touching it.** Its GATT
callback is a *mailbox*, never an executor: the beep helpers block for hundreds
of ms (stalling the NimBLE host task, including the connection awaiting that
write's ATT response) and the relay belongs to the app task, where two owners on
one GPIO is how a relay latches on with nobody responsible. `loop()` executes.
Physical ops are refused — dropped, never deferred — unless `app_state_is_idle()`.
And `sync_ack` is recorded, never written into `counters_set_last_seq()`: it
carries the *backend's* seq, which can be lower than the board's.

Two contract facts that look like arbitrary choices and are not: **time is UTC**
(`epoch_utc:u32`), and the **business day is Asia/Manila**, rolling at 16:00 UTC.
The app displays the board's `today_amount` *instead of* the backend's figure
while connected, so a UTC day boundary would make the number jump on connect.

## The RTC is a power-loss backup, not the running clock

`src/rtc.*` drives the SLM1302 on IO25/26/4. It seeds the system clock at boot;
`time()` runs everything after that, and the system clock is written back hourly.
Do not put an RTC read in a per-coin path — it is ~350 us of bit-banging.

**BT1 is a CR2032. Trickle charging can vent it**, so `rtc_init()` writes register
`0x90` to zero every boot *and reads it back*. Enabling it requires two macros and
is a compile error otherwise. Treat that as a safety property, not a setting.

## Pins and strapping

`include/pins.h` is the single source, with names matching the STM32's. Four
places are **not** one-to-one: `PIN_LED_A` (no counterpart — one debug LED, not
two), `PIN_BTN3` (no counterpart — gestures remapped), `PIN_SENSOR_IN` (PB0 →
J6 AUX_1 IO32, no dedicated pin exists; IO32 is not 5V tolerant, check JP9), and
`PIN_WDT_DONE` (IO2, new — the STM32's watchdog had no pin).

**Three of our pins are strapping pins sampled at reset**, which the STM32 had no
equivalent of:

- **IO12 (`COIN_EN`)** — high at reset can force 1.8V flash timing and stop the
  board booting. Never idle it high.
- **IO2 (`WDT_DONE`)** — high at reset picks an unintended boot mode. Driven low
  before `pinMode(OUTPUT)`, pulsed, returned low.
- **IO15 (`LED_B`)** — gates the ROM boot log (cosmetic), but shares its node with
  the internal 45K boot pull-up.

So: **every MOSFET gate is written LOW before `pinMode(OUTPUT)`** (`out_safe_low()`),
and **`periph_reset_safe()` must be called before any `esp_restart()`**. External
resistors cover a cold power-on; only firmware covers a warm restart.

Buttons use plain `INPUT`, never `INPUT_PULLUP` — each has its own external
pull-up, and IO34/IO35 are input-only pads with no internal pull-up at all.

## Inherited behaviour that is NOT a bug to fix here

**Pause gives accumulated credit no protection.** In `OP_PAUSE_RESUME` the
deadline advances in real wall-clock time whether the relay is engaged or not
(intentional, per spec), and Accumulation is blind to that — a coin inserted while
paused is billed into a deadline that keeps running out. A customer who pauses,
inserts, and doesn't resume within `relay_on_ms` gets nothing for it.

This is faithfully replicated from the STM32, where it is logged as item 2 of
`BUGS_HANDOVER.md` and explicitly flagged as **needing a product decision, not a
code fix**. The real answer is `OP_ON_OFF_TOGGLE` (enum 2, reserved and
unimplemented on both platforms). Don't unilaterally change it here — that would
diverge the two firmwares on a money-handling rule.

Also inherited as stubs on both sides, don't treat as missing work: GSM reporting,
the 7-segment display type, and the config-mode beeps.

## Out of scope (for now)

The **master/slave daisy-chain protocol** is a later phase (user's decision). The
board has the hardware — JP10 role strap on IO5, `I2C_INT` wired-OR on IO16,
daisy headers J3/J4 — and this firmware reserves both pins, reports the strap via
`AT+STRAP?`, and drives neither. `PIN_I2C_INT` is open-drain **only** when it
lands: two boards driving it push-pull is a direct short.

The protocol exists on the STM32 side on branch `ckabiling/slave_non_RTOS` (config
push `0xC3`, credit broadcast `0xC4`, dispense request/grant `0xC5`, `max_credit`
allowance, dynamic discovery); design notes are in
`../Vendo_I2C_Master_Slave/CLAUDE.md` and memory
`slave-dispense-parity-architecture`. It needs one bench number first: `I2C_INT`
has R32 100K into C29 100nF, predicting a **~7 ms rise** — slow enough that any
request/grant protocol must allow for it. See `docs/PENDING.md` item 6.

The **RTC (U6, SLM1302** on IO25/IO26/IO4) is S1-only hardware with no STM32
counterpart, so nothing was ported and this firmware doesn't touch it.

## Docs in this repo

| | |
| --- | --- |
| `README.md` | Operator/dev facing: modes, features, menu, CLI |
| `docs/PORTING_FROM_STM32.md` | What diverged from the STM32 and why; pin table; the 10 inherited bug fixes |
| `docs/PENDING.md` | **The live task list.** Watchdog supervision (1), TPL5010 timeout (2), coin polarity (3) first |
| `docs/REVIEW_FINDINGS.md` | Code-review defects and hardening, stable IDs `R1`–`R22`. Read **R5**–**R8** before touching `wdt.cpp` — R7 is why PENDING item 1's one-line fix is not yet safe |
