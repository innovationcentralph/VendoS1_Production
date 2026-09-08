# Vendo S1 Production Firmware — Pending Items

Open work carried by this port, newest concerns first. Items 1–3 are the ones
that were consciously deferred while porting; items 4+ are bench-verification
tasks that only real hardware can close.

> **Companion list:** `docs/REVIEW_FINDINGS.md` tracks defects found by code
> review (IDs `R1`–`R22`). This file is what the *port* deferred; that one is
> what *reading the code* turned up. Four of its findings bear directly on the
> items below — see the cross-references in items 1 and 2.

---

## 1. [HIGH] Watchdog does not supervise the application yet

**Files:** `src/wdt.h` (full rationale), `src/wdt.cpp` (`wdt_task_run`, the
commented-out enforcement block)

**What is deferred.** The TPL5010 is petted **unconditionally** every 100 ms by a
dedicated high-priority task. That catches total system death — panic, brownout,
core lockup, crash — but **not an application-level hang**. If `app_task_run`
wedged while the wdt task kept running, the board would sit there petting
happily forever and present as a dead-but-powered machine.

**What we lost relative to the STM32.** The STM32 used its internal IWDG with a
~25 s timeout, kicked by `watchdog_feed()` calls inside the app task, session
countdown, config menu and beep helper. Because those loops all ran far faster
than 25 s, an app-task hang stopped the feeding and reset the MCU. The watchdog
supervised the *application*, not just the chip. This port does not, yet.

**Why it was deferred rather than just implemented.** Two reasons:

1. The real TPL5010 timeout is still unmeasured (item 2 below). Choosing a
   liveness deadline without it means guessing, and a deadline guessed too tight
   reboots the board **mid-vend** — strictly worse than the supervision gap it
   would close.
2. The 100 ms pet cadence the user asked for is a hardware constraint, not a
   policy choice, so it had to be decoupled from application progress regardless.
   Feed calls sprinkled through app code cannot meet a ~100 ms budget when the
   app legitimately blocks for 2 s (boot splash), 3 s ("COMPLETE!" hold), 600 ms
   (denied message) or a multi-beep pattern — all of which were free under a 25 s
   IWDG.

**The scaffolding is already in place.** Every STM32 `watchdog_feed()` call site
was ported and now stamps a per-source timestamp (`WDT_SRC_APP`,
`WDT_SRC_SESSION`, `WDT_SRC_MENU`), and `watchdog_liveness_stale_ms()` reports
the worst offender. `AT+WDT?` shows all of it live. Enforcement is one `if` in
`wdt_task_run()`, at the marked spot:

```c
if (watchdog_liveness_stale_ms() > WDT_LIVENESS_DEADLINE_MS) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));   // stop petting -> TPL5010 resets
}
```

**To close it:** measure the timeout (item 2), then set the deadline from the
longest legitimate blocking stretch in the app with real margin. The longest
today is the 3 s session-complete hold, so something in the 8–10 s region is the
starting point — but note that `session_run` checks in *during* that hold, so the
true worst case is per-loop, not per-state. Confirm against `AT+WDT?` readings on
a machine doing real vends before turning it on.

> ⚠️ **Read `REVIEW_FINDINGS.md` R7 before writing that `if`.** As drafted above it
> reboots *healthy* boards: `watchdog_liveness_stale_ms()` returns the worst age
> across all sources, and `WDT_SRC_SESSION`/`WDT_SRC_MENU` legitimately stop
> checking in when their state exits — so the session slot ages without bound
> after the first vend. Sources must deregister on exit, or enforcement must look
> only at `WDT_SRC_APP`.
>
> **R6 also argues this item does not have to wait on item 2 at all.** The ESP32's
> own task watchdog is already compiled in (5 s, panic-on-expiry) and currently
> unused — `esp_task_wdt_add()`/`esp_task_wdt_reset()` gives application
> supervision now, independent of the external window. R5 is the related
> correction: the "core-1 idle gives us free supervision" claim in `main.cpp` and
> `CLAUDE.md` is false for this framework build.

---

## 2. [HIGH] The TPL5010 timeout is unknown and must be measured

**Why it matters.** Everything above depends on it, and so does whether 100 ms is
actually a safe pet interval or merely a plausible one. The timeout is programmed
by **R34 40.2K** to GND, with **R33 10K + solder jumper JP14** as a stuff option,
and firmware cannot read it: the board wires `DONE` but leaves `WAKE`
unconnected, so there is no signal telling us when the window opens. We can only
pulse DONE faster than the timeout and hope.

**How to measure it** — the S1 bring-up harness already has the rig for this.
In `../Vendo_S1_TestCode`:

```
AT+WDT=0        <- stop petting; the board will reset
                   (wait for the banner)
AT+INFO?        <- reports how long it survived
```

`AT+WDT_MS=<n>` brackets it from the other side: raise the interval in steps with
petting on, and the first value that lets the board reset is just over the real
window.

**Caveat carried over from the harness:** the TPL5010 resets via the EN pin, and
pulling EN low may power down the RTC domain too — in which case the duration
measurement is lost and only the reset *reason* survives. That still confirms the
watchdog fired; time it with a stopwatch instead.

**Then:** if the measured window is comfortably above 100 ms, consider relaxing
the pet interval — 100 ms was chosen to sit inside even the shortest *plausible*
window (the part's range bottoms out near there), not because it is known to be
needed. **No reset at all** means the watchdog is bypassed — check JP14 and
whether R33/R34 are stuffed as drawn.

> **`REVIEW_FINDINGS.md` R8 raises this item's priority.** 40.2K sits well up the
> TPL5010's resistor/interval table, nowhere near the few-hundred-ohm end that
> selects the 100 ms minimum — so the real window is very likely seconds to
> minutes. If so, several constraints this codebase is built around (no blocking
> in the vend path, no petting from application code, petting confined to its own
> task) relax considerably. Check the datasheet table, then measure. Don't relax
> anything on the inference alone.

---

## 3. [HIGH — money] Coin pulse polarity is a guess until bench-confirmed

**Files:** `AppConfig::coin_active_high` in `src/config.h`, `coin_counter_task_run`
in `src/periph.cpp`, `AT+COIN_POLARITY` / `AT+COIN?` in `src/cli.cpp`

The STM32 hard-coded "idle LOW, pulse HIGH" because its acceptor wiring was
known. On the S1 the pulse passes through the **PC817 (U4)** opto and the
schematic does not say which way its output swings — R22/R25 1K are the populated
bias resistors, R23/R26 1K are unpopulated alternates for a different
voltage/polarity. The bring-up harness had to make its counting edge switchable
for exactly this reason, and defaults to FALLING.

So this port defaults to **active-LOW** (idle HIGH, pulse LOW) and makes it a
config field rather than baking a guess into the build. **This is the coin input
— getting it wrong means the machine takes money and counts nothing, or counts
pulses that never happened.** Confirm before shipping any board:

```
AT+COIN?                 <- shows live level, polarity, and pulse count
                            drop a coin; the count must rise
AT+COIN_POLARITY=1       <- flip it (applies live, no reset needed)
```

Once confirmed on real hardware, consider changing the factory default in
`kDefaults` (`src/config.cpp`) so a fresh board is right out of the box.

---

## 4. [MEDIUM] Verify `digitalRead(SDA)` reflects the pad while Wire owns the pin

**File:** `i2c_needs_recovery()` in `src/display.cpp`

The stuck-bus check reads the SDA pad directly while the I2C peripheral is
driving it — ported from the STM32, which did the same. On the ESP32 the pin is
routed through the GPIO matrix, and `digitalRead()` reads `GPIO_IN`, which should
reflect the pad because the I2C driver keeps the input path enabled (it has to
sample ACK). Expected to work, but not yet confirmed on hardware.

Both failure modes are loud and easy to spot, which is why this is MEDIUM rather
than HIGH:

- **always reads LOW** — a recovery fires on every display message and the LCD
  visibly thrashes
- **always reads HIGH** — a genuinely stuck bus is never detected, so the LCD
  stays garbled instead of healing (the NAK-count check in the same function
  still catches most of this case)

---

## 5. [MEDIUM] Sensor Stop input lives on an AUX pin — confirm the rail

**File:** `PIN_SENSOR_IN` in `include/pins.h`

The STM32 had a dedicated sensor input (PB0). The S1 has none, so Sensor Stop
reads **AUX_1 (IO32)** on the J6 expansion header. Two things to check when the
feature is first used:

- **J6's rail is jumper-selected by JP9** (+3.3V or +5V) — it must match the
  sensor. IO32 is not 5V tolerant, so a 5V sensor output needs dividing or
  level-shifting even if the sensor is *powered* from +5V.
- The pin is configured `INPUT_PULLUP`, which suits a dry-contact-to-ground
  sensor and matches the active-LOW default. A sensor that drives the line
  actively wants the pull-up dropped.

---

## 6. [LOW] Master/slave daisy-chain protocol not ported

Deliberately out of scope for this pass (confirmed with the user). The S1 board
has the hardware for it — JP10 role strap on IO5, `I2C_INT` wired-OR line on
IO16, daisy headers J3/J4 — and this firmware reserves both pins, reports the
strap via `AT+STRAP?`, and drives neither.

The protocol exists on the STM32 side on branch `ckabiling/slave_non_RTOS`
(config push `0xC3`, credit broadcast `0xC4`, dispense request/grant `0xC5`,
`max_credit` allowance, dynamic slave discovery). Porting it needs one bench
number first: `I2C_INT` has R32 100K into C29 100nF, predicting a **~7 ms rise
time**, which is slow enough that any request/grant protocol has to allow for it
or a board polling faster will read a stale low. `AT+INT=M` in the bring-up
harness measures it.

---

## 7. [LOW] Carried over from the STM32: pause gives accumulated credit no protection

Not introduced by this port — the behaviour is faithfully replicated from the
STM32, where it is logged as item 2 of `BUGS_HANDOVER.md` and flagged as needing
a **product decision, not a code fix**.

In `OP_PAUSE_RESUME` the deadline keeps advancing in real wall-clock time whether
the relay is engaged or not (intentional, per spec). Accumulation is blind to
that: a coin inserted *while paused* is billed and added to `session_ticks`
identically to one inserted while running, and that newly-purchased time then
ticks away with the relay off. A customer who pauses, inserts a coin, and does
not resume within `relay_on_ms` gets nothing for it.

The real fix is `OP_ON_OFF_TOGGLE` (enum value 2, reserved and unimplemented on
both platforms) — a pause that also freezes the deadline. Options are laid out in
the STM32 `BUGS_HANDOVER.md`; whoever owns the spec should pick a direction
before either codebase changes.

---

## 8. [LOW] Not ported because they were stubs on the STM32 too

- **GSM reporting** — `AppConfig::gsm_reporting_enabled` is honoured as a flag
  and the call site exists in `APP_STATE_SESSION_END`, but there is no
  `gsm_report_send()` on either platform.
- **7-segment display** — `DISPLAY_7SEG_4DIG` is selectable and logs "stub".
  `PIN_7SEG_CLK`/`PIN_7SEG_DIO` (IO13/IO14) are declared so they cannot be reused
  by accident. TM1637-style is only an *inference* from the 2-wire CLK/DIO shape;
  the module type needs confirming before anyone writes a driver.
- **Config-mode beeps** — `buzzer_beep_enter_config()` /
  `buzzer_beep_exit_config()` are empty on both platforms.

## 9. [LOW] S1-only hardware with nothing to port

The **RTC (U6, SLM1302)** on IO25/IO26/IO4 is capability the STM32 board does not
have, so no behaviour was ported and this firmware does not touch it. If session
timestamping or scheduled operation is ever wanted, the bring-up harness already
has a working 3-wire driver (`../Vendo_S1_TestCode/src/s1_rtc.cpp`), including
the warning that `AT+RTC_TRICKLE` must stay off unless BT1 is a rechargeable
LIR cell — trickle-charging a plain CR2032 damages or vents it.
