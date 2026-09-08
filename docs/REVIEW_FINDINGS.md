# Vendo S1 Production Firmware — Review Findings Register

Defects and hardening opportunities found by **code review**, tracked with stable
IDs so they can be referenced from commits, and so a finding that gets deferred
stays visible instead of being rediscovered.

- **First pass:** 2026-08-24, against `69260ad` (whole tree, ~4.3k lines).
- **Line references are as of that commit** and will drift — the function names
  in each **Files** line are the durable pointer.
- **Nothing here was observed on hardware.** Same caveat as the rest of the repo
  (see `CLAUDE.md`): this firmware has not run on a board. Every finding is read
  from the source or verified against the pinned toolchain, and each one says
  which.

## How this file relates to `docs/PENDING.md`

They are deliberately separate lists:

| | |
| --- | --- |
| `docs/PENDING.md` | Work the **port** consciously deferred, plus bench measurements only real hardware can close. Written before review. |
| `docs/REVIEW_FINDINGS.md` (this file) | Defects and hardening found by **reading the code**. Written after. |

Where they overlap, this file links to the PENDING item rather than restating
it — **R5**–**R8** all bear on PENDING item 1 (watchdog supervision) and item 2
(TPL5010 timeout), and **R7** in particular says why PENDING item 1's proposed
one-line fix is not yet safe to switch on.

## Status legend

`[ ]` open · `[~]` in progress · `[x]` fixed · `[-]` closed, won't fix (record why)

---

## Summary

| ID | Sev | Area | Finding | Status |
| --- | --- | --- | --- | --- |
| R1  | HIGH   | money    | `price_per_credit_cents` can be set to 0 — machine banks P0.00 per coin | `[ ]` |
| R2  | HIGH   | money    | Sensor Stop has no arming requirement — a stuck sensor ends every session in ~60 ms | `[ ]` |
| R3  | HIGH   | money    | `blocks * relay_on_ms` has no clamp — `uint32_t` overflow can shorten a paid session | `[ ]` |
| R4  | HIGH   | money    | Banked money is RAM-only — any reset silently zeroes a customer's balance | `[ ]` |
| R5  | HIGH   | watchdog | The "free supervision from core-1 idle" claim is false for this framework build | `[ ]` |
| R6  | HIGH   | watchdog | PENDING item 1 can be closed **today** with the already-built ESP32 TWDT | `[ ]` |
| R7  | HIGH   | watchdog | PENDING item 1's proposed enforcement `if` will reboot healthy boards | `[ ]` |
| R8  | MEDIUM | watchdog | R34 = 40.2K suggests the real TPL5010 window is far longer than 100 ms | `[ ]` |
| R9  | MEDIUM | stuck    | `config_menu_run()` never times out, and holds the coin slot inhibited | `[ ]` |
| R10 | MEDIUM | stuck    | `WAIT_NEXT_CREDIT` has no exit under the **default** config | `[ ]` |
| R11 | MEDIUM | ui       | BTN1+BTN2 suppression misses the first button's press-confirm | `[ ]` |
| R12 | MEDIUM | display  | Idle screen clears and redraws every 5 s, indefinitely | `[ ]` |
| R13 | MEDIUM | display  | `s_recovery_fails = 0` defeats the 3-strike LCD disable | `[ ]` |
| R14 | LOW    | display  | `lcd_autodetect()` fallback will latch onto a sibling board on the daisy-chain | `[ ]` |
| R15 | MEDIUM | reset    | Shutdown handler drops only the relay, not the strapping pins | `[ ]` |
| R16 | LOW    | diag     | `wdt_task_run()`'s banner is printed before `Serial.begin()` and is lost | `[ ]` |
| R17 | LOW    | diag     | No stack high-water reporting | `[ ]` |
| R18 | MEDIUM | config   | No range validation on a loaded config blob | `[ ]` |
| R19 | LOW    | config   | No `static_assert` on `sizeof(AppConfig)` | `[ ]` |
| R20 | LOW    | robust   | `coin_counter_init()`'s mutex creation is unchecked | `[ ]` |
| R21 | LOW    | style    | `PIN_USER_LED` is written directly from `app.cpp` | `[ ]` |
| R22 | MEDIUM | policy   | `ENABLE_CLI` on a field unit is a free-vend backdoor at J7 | `[ ]` |

Build at time of review: **RAM 6.7%** (22 080 / 327 680 B), **Flash 24.1%**
(315 961 / 1 310 720 B). There is no size pressure on this board — every
"performance" finding below is about latency, blocking, or field robustness,
never footprint.

---

# Money and customer-facing correctness

These four are the ones that can take a customer's coin and give nothing back.

## R1. [HIGH] `price_per_credit_cents` can legitimately be set to 0

**Files:** `config_menu_run()` page 9 in `src/app.cpp:284-285`,
`cmd_price_per_credit()` in `src/cli.cpp:427`, `coin_counter_task_run()` in
`src/periph.cpp:255`

The LCD menu floors page 9 to `0`, and `AT+PRICE_PER_CREDIT` accepts `0..99999`.
At zero, every coin pulse adds P0.00 to the running total:

```c
s_coin_value_cents += s_price_per_credit_cents;   // += 0
```

`ready` never becomes true, so the machine accepts coins forever while showing
"INSERT COIN". Nothing in the log says why.

This is the most likely operator-caused field failure in the repo, and it is
reachable with two button presses on the front panel — no CLI, no reflash.

**Fix:** floor both paths at one step (`PRICE_STEP` in the menu, `1` in the CLI).
A price of zero has no legitimate meaning on a coin-op machine.

## R2. [HIGH] Sensor Stop has no arming requirement

**Files:** `session_run()` in `src/session.cpp:200-212`

The trigger check runs from the first loop iteration with no requirement that the
sensor was ever *inactive* first. If `PIN_SENSOR_IN` already reads its active
level when the session starts, `sensor_active_ms` crosses `sensor_wait_ms`
immediately — ~60 ms at the default 50 ms — and the session ends. The money was
already consumed in `APP_STATE_COIN_VALIDATE` before `session_run()` was called.

Ways the pin reads active at session start, all of them ordinary field faults:

- sensor failed closed, or its wiring shorted
- `sensor_active_high = 1` configured while nothing is connected (IO32 is
  `INPUT_PULLUP`, so it floats HIGH = active)
- wrong JP9 rail, or a 5 V sensor output on the not-5V-tolerant IO32

The default `sensor_active_high = false` plus the internal pull-up means a
*disconnected* sensor reads inactive, which is the safe direction — so this only
bites once someone enables the feature and something goes wrong with it. That is
exactly when you least want the failure mode to be "silently keeps the money".

**Fix:** require one inactive tick before the trigger arms. Optionally refuse to
start the session at all while the sensor reads active, and log it — a machine
that says "SENSOR FAULT" is diagnosable; one that vends 60 ms blocks is not.

**Cross-check the STM32 first** — per `CLAUDE.md`'s diffability rule, if the
upstream has the same shape this fix probably belongs on both sides.

## R3. [HIGH] `blocks * relay_on_ms` has no clamp

**Files:** `session_run()` in `src/session.cpp:108`

```c
const uint32_t total_ms = blocks * cfg->relay_on_ms;
```

`blocks` comes from `coin_get_value_cents() / price_cents`, and the banked total
is itself unbounded — nothing caps `s_coin_value_cents`. With `relay_on_ms` at
its documented 3 600 000 ms maximum, this overflows `uint32_t` at ~1194 blocks,
and a wrapped product can come out **small**: the customer pays for hours and
gets seconds.

`SESSION_TICKS_CAP` (7 days) guards the *accumulation* extension path in the same
file and is carefully written to avoid its own overflow — the initial computation
never got the same treatment.

Reaching it needs a runaway pulse source (EMI on the coin line, a chattering
acceptor), which is a real field mode and is also worth detecting in its own
right.

**Fix:** cap `blocks` against `SESSION_TICKS_CAP / relay_on_ms` before the
multiply, and cap the banked total in `coin_counter_task_run()`. A ceiling on
banked money doubles as a runaway-pulse detector worth logging.

## R4. [HIGH — product decision] Banked money is RAM-only

**Files:** `s_coin_value_cents` in `src/periph.cpp:212`

The customer's balance exists only in RAM. Any reset zeroes it silently — and on
this board resets are an expected event, not an exception:

- the TPL5010 is unmaskable and its window is still unmeasured (PENDING item 2)
- `CONFIG_ESP32_BROWNOUT_DET_LVL 0` — brownout detection at the least sensitive
  threshold, on a 3.3 V rail that shares a supply with a relay coil
- any panic, and any `AT+RESET`

Every one of those presents to the customer as the machine eating their money.

**This is a product decision as much as a code one**, which is why it is recorded
rather than simply fixed: persisting per-coin costs NVS wear, and the right
answer may be "accept the risk" or "persist on a debounce". Worth deciding
deliberately rather than by default.

**Fix if taken:** write the balance to NVS on change, debounced (e.g. 2 s of no
coin activity), and restore it at boot. NVS wear-levels, so this is far cheaper
here than the equivalent would have been on the STM32's emulated EEPROM.

---

# Watchdog and supervision

**This section is the highest-leverage part of the review.** R5 corrects a
factual error the design rests on, R6 is an opportunity to close PENDING item 1
now, and R7 is a landmine in the fix PENDING item 1 currently proposes.

## R5. [HIGH] The "free supervision from core-1 idle" claim is false for this build

**Files:** `src/main.cpp:29-31`, and the same claim in the **Task layout** section
of `CLAUDE.md`

Both say:

> Pinning them together also means a task that spins without yielding starves
> core 1 idle, which trips the ESP32's own task watchdog — free supervision we
> would lose by spreading them.

**Verified against the pinned toolchain** — `framework-arduinoespressif32`
`3.20017.241212` (Arduino 2.0.x),
`tools/sdk/esp32/qio_qspi/include/sdkconfig.h`:

```
#define CONFIG_ESP_TASK_WDT 1
#define CONFIG_ESP_TASK_WDT_PANIC 1
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S 5
#define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 1
```

`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1` is **not defined**. Core 1's idle task
is not subscribed, and this firmware runs nothing on core 0 — so the task
watchdog currently supervises nothing this firmware does. There is no free
supervision to lose.

(`CONFIG_ESP_INT_WDT_CHECK_CPU1 1` *is* set, but the interrupt watchdog catches
only interrupts-disabled-too-long, not a wedged task.)

**Fix:** correct both comments, and see R6 for what to do instead. Note the
pinning decision itself is still right — leaving core 0 for WiFi/BLE is a good
reason on its own. Only the supervision justification is wrong.

## R6. [HIGH] PENDING item 1 can be closed today, without measuring the TPL5010

**Files:** `wdt_task_run()` in `src/wdt.cpp`, every `watchdog_feed()` call site

PENDING item 1 defers application supervision because choosing a liveness
deadline needs the TPL5010 number from item 2. That reasoning holds for the
*external* watchdog — but the ESP32 has a second, independent one already
compiled in and currently unused (see R5): TWDT, 5 s, panic-on-expiry.

A TWDT panic reboots the board and lands as `ESP_RST_TASK_WDT`, which
`wdt_print_boot_report()` **already decodes and prints at boot**. The diagnostics
for this are done.

```c
esp_task_wdt_add(NULL);      // from app_task_run, after the boot splash
esp_task_wdt_reset();        // alongside each existing watchdog_feed()
```

Budget check against the 5 s timeout — the longest stretches without a check-in
today are the 2 s boot splash and the 3 s "COMPLETE!" hold in `session_run()`.
Both fit, provided the app task subscribes *after* the splash. Everything else
checks in on a 20 ms loop.

This gives genuine application supervision now, decoupled from the unknown
external window, and leaves the TPL5010 doing what it is good at — catching total
system death.

## R7. [HIGH] PENDING item 1's proposed enforcement will reboot healthy boards

**Files:** `watchdog_liveness_stale_ms()` in `src/wdt.cpp:93`, the commented-out
enforcement block in `wdt_task_run()`

PENDING item 1 describes the remaining work as "one `if`". As written, that `if`
bricks the board:

```c
if (watchdog_liveness_stale_ms() > WDT_LIVENESS_DEADLINE_MS) { /* stop petting */ }
```

`watchdog_liveness_stale_ms()` returns the **worst** age across every source that
has ever checked in. It correctly skips sources that have never run — but
`WDT_SRC_SESSION` and `WDT_SRC_MENU` legitimately *stop* checking in the moment
their state exits. After the first vend, the session slot ages without bound, and
the board crosses any deadline while sitting perfectly healthy in IDLE.

The "never checked in is not the same as stalled" rule handles boot. It does not
handle "ran, finished, and is now idle by design" — which is the steady state for
two of the three sources.

**Fix before enabling either:** deregister on exit (stamp `0` when a state is
left), or enforce only on `WDT_SRC_APP`, the one source that is always running.
Keep the other two as diagnostics for `AT+WDT?`.

## R8. [MEDIUM] R34 = 40.2K suggests the real window is far longer than 100 ms

**Files:** `WDT_PET_MS` in `src/wdt.h` · relates to **PENDING item 2**

`WDT_PET_MS 100` was chosen to sit inside even the shortest *plausible* TPL5010
window, which the header describes as bottoming out "around 100 ms". But 40.2K
sits well up the TPL5010's resistor/interval table — nowhere near the
few-hundred-ohm end that selects the minimum interval. The real window is very
likely seconds to minutes.

That matters beyond efficiency: the ~100 ms assumption is the stated reason the
codebase cannot use ordinary blocking calls in the vend path, cannot pet from
application code, and had to move petting to a dedicated task. If the true window
is tens of seconds, several of those constraints relax and the STM32's original
IWDG-shaped design becomes portable again.

**Do not relax anything on this reasoning alone** — confirm against the datasheet
table, then measure per PENDING item 2. This finding exists to raise that item's
priority, not to pre-empt it.

---

# Stuck states

Each of these leaves a powered machine that takes no money, with nothing on the
display or the log to explain it.

## R9. [MEDIUM] `config_menu_run()` never times out

**Files:** `config_menu_run()` in `src/app.cpp:148`, `APP_STATE_CONFIG_MODE` in
`src/app.cpp`

The menu loop is a bare `for (;;)` whose only exits are the save combo and the
cancel long-press. `APP_STATE_CONFIG_MODE` calls `coin_slot_disable()` for the
whole duration — correct, since no coins should be taken mid-edit, but it means
an operator interrupted halfway through leaves a machine that is dead to money
until someone comes back and presses the right buttons.

**Fix:** auto-cancel (discarding edits, the safe direction) after a few minutes
with no button activity. `watchdog_feed(WDT_SRC_MENU)` already runs every
iteration, so the loop has the tick source it needs.

## R10. [MEDIUM] `WAIT_NEXT_CREDIT` has no exit under the default config

**Files:** `APP_STATE_WAIT_NEXT_CREDIT` in `src/app.cpp:635,667`

```c
const bool timeout_elapsed = cfg.inactivity_timeout_s > 0 && /* ... */;
/* ... */
if (start_button_pressed() || timeout_elapsed) { /* next block */ }
```

`inactivity_timeout_s` defaults to **0 = disabled**. With per-credit gating on and
that default, the only way out of this state is a START press. A customer who
takes their first block and walks away parks the machine on "PRESS FOR NEXT
CREDIT" permanently — USER_LED lit, coin slot live, never returning to IDLE.

The two settings are individually reasonable and only combine badly. Nothing in
the menu warns about the combination.

**Fix:** either require a non-zero `inactivity_timeout_s` whenever
`press_to_start_per_credit` is enabled (enforce at save time in both the menu and
the CLI), or apply a hard ceiling in this state regardless of the setting.

---

# UI

## R11. [MEDIUM] BTN1+BTN2 suppression misses the first button's press-confirm

**Files:** `btn_b1_pressed()` / `btn_b2_pressed()` in `src/periph.cpp:409-417`

The suppression works by checking whether the *other* button is currently down:

```c
bool btn_b1_pressed() {
    const bool fired = btn_simple_poll(PIN_BTN1, &s_btn1, true);
    return fired && !other_btn_down(PIN_BTN2);
}
```

That covers everything after both buttons are down. But a press confirms after 3
polls (~60 ms), while the combo needs ~1.5 s of both — so no human presses them
close enough together to avoid the first button firing alone. On the boolean
pages (Per Credit, Accumulation, Sensor Stop, Sensor Polarity) that write is then
committed by the save the operator was in the middle of performing.

This is the same class of bug the suppression was added for, just inverted: the
comment in `periph.cpp` explains how the combo would silently force those
settings **ON**; as it stands it silently forces whichever value the
first-touched button writes — usually **OFF**, since BTN1 tends to land first.

**Fix:** snapshot the current page's value on entry to the combo's `B12_DEBOUNCE`
state and restore it if the combo goes on to fire; or require a fired press to be
re-armed once the combo state machine leaves `B12_IDLE`.

---

# Display

## R12. [MEDIUM] The idle screen clears and redraws every 5 seconds, indefinitely

**Files:** `display_task_run()` heartbeat in `src/display.cpp:607`,
`apply_display_defaults()` in `src/lcd.cpp:80`

The no-message heartbeat calls `lcd_reinit()`, which runs
`apply_display_defaults()`, which issues **Clear Display (0x01)** before the
re-render.

`APP_STATE_IDLE` only posts a message when `money_cents` changes. With no coins
in the machine nothing posts — so the heartbeat fires every 5 s forever, and the
machine's normal 24/7 resting state is a display that blanks and redraws on a
5-second cycle. To an operator that reads as a fault.

The heartbeat's stated purpose is healing garble that did not trip an I2C error,
which is worth keeping — it is the unconditional `lcd_reinit()` and its clear
that are the problem.

**Fix:** re-render on the heartbeat without the reinit, and reinit only on
evidence of trouble (a non-zero NAK count) or every Nth heartbeat. Secondary
benefit: `enter_4bit_mode()` busy-waits ~13.5 ms in `delayMicroseconds()` with no
yield, which stops being a periodic cost.

## R13. [MEDIUM] `s_recovery_fails = 0` defeats the 3-strike LCD disable

**Files:** `display_task_run()` in `src/display.cpp:654`,
`i2c_recover_and_reinit()` and `lcd_hw_init()` in the same file,
`lcd_autodetect()` in `src/lcd.cpp:109`

```c
if (/* IDLE msg */ && !s_lcd_ok) {
    s_recovery_fails = 0;
    i2c_recover_and_reinit();
}
```

The counter is zeroed immediately before each attempt, so it can never reach 3
from this path — `i2c_recover_and_reinit()` increments it to 1 and returns. The
"LCD disabled after 3 failed recoveries" policy documented in `CLAUDE.md` never
takes effect for a board that starts out with no working display.

The cost lands in `lcd_hw_init()`: a failed preferred-address probe falls through
to `lcd_init(0)` -> `lcd_autodetect()`, which sweeps 0x08..0x77. On a bus held
low — unstuffed JP1/JP2, or no LCD at all — each of those 112 probes can burn the
full 50 ms `Wire.setTimeOut()`, so one sweep is multiple seconds.

**This matters more than it looks**: `display_type` defaults to
`DISPLAY_LCD_16X2`, and "no LCD fitted" is a perfectly normal configuration for a
board being sold as a generic timer. Such a board pays a multi-second bus sweep
on idle redraws.

**Fix:** let the strike counter actually reach its limit, and back off retries
(exponential, or once per N minutes) instead of retrying per IDLE message. A
board with no display should settle into cheap silence.

## R14. [LOW — blocks a later phase] `lcd_autodetect()` fallback will latch onto a sibling board

**Files:** `lcd_autodetect()` in `src/lcd.cpp:109`

The fallback returns the first address that ACKs anywhere in 0x08..0x77. The
comment acknowledges this is a guess on a daisy-chain, and `lcd_hw_init()` warns
on the log line when it lands somewhere unusual — but it still *proceeds*, and
then writes HD44780 nibbles at whatever answered.

On the daisy-chain bus this firmware already reserves for master/slave (J3/J4,
PENDING item 6), that will be a sibling Vendo board, which receives a stream of
what is to it meaningless data.

**Fix:** before the master/slave phase lands, restrict the fallback to plausible
backpack addresses, or drop it and require the address to be configured. Harmless
today because nothing else is on the bus — recorded so it is not discovered the
hard way when the second board arrives.

---

# Reset safety

## R15. [MEDIUM] The shutdown handler drops only the relay

**Files:** `relay_off_on_shutdown()` in `src/main.cpp:65`, registered at
`src/main.cpp:109`; `periph_reset_safe()` in `src/periph.cpp:73`;
`cmd_reset()` in `src/cli.cpp`

`periph_reset_safe()` exists precisely because three of this board's pins are
strapping pins sampled at reset, and IO12 high at reset can stop the board
booting at all. But the registered shutdown handler only does the relay, so
`periph_reset_safe()` runs on exactly one path: `cmd_reset()` calling it
manually, 20 ms before `esp_restart()`.

Inside that 20 ms window every other task is still running:

- the app task can call `coin_slot_enable()` and drive IO12 back HIGH
- the wdt task pulses IO2 HIGH every 100 ms (5 us wide, so a small window — but a
  nonzero one, and the consequence is an unintended boot mode)

**Fix:** make the shutdown handler `periph_reset_safe()` itself. It is three
`digitalWrite`s and a `pinMode` — no allocation, no logging, no locks — so it
satisfies the same shutdown-context constraints the current bare handler was
written for, and it covers **every** `esp_restart()` path rather than one. The
manual call in `cmd_reset()` then becomes belt-and-braces instead of the only
line of defence.

---

# Diagnostics, config hardening, and smaller items

## R16. [LOW] `wdt_task_run()`'s banner never reaches the UART

**Files:** `wdt_task_run()` in `src/wdt.cpp`, `setup()` in `src/main.cpp`

The wdt task is created at priority 6 — above `loopTask` — so it preempts
`setup()` immediately and prints **before** `Serial.begin(115200)` on the next
line.

**Verified in the pinned core:** `HardwareSerial::write` calls `uartWriteBuf`,
which opens with `if (uart == NULL || ...) return;`, and `_uart` is NULL until
`begin()`. So the call is safe but silently discarded — the boot log never
confirms that petting started, which is the one thing you want to see first when
a board is resetting in the field.

**Fix:** move the print after the first `vTaskDelayUntil()`, or have `setup()`
print it once `Serial` is up.

## R17. [LOW] No stack high-water reporting

**Files:** `wdt_print_status()` in `src/wdt.cpp`, `cmd_wdt()` in `src/cli.cpp`

`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY` and
`CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` are both on, so an overflow panics
loudly and lands as `ESP_RST_PANIC` — you learn *that* it happened, never how
close anything was.

Stack sizes here were hand-converted from the STM32's word counts to bytes (see
the note in `main.cpp`), which is exactly the kind of change worth having
evidence for. `display_task_run` is the one to watch: 4096 B against `snprintf`
plus Wire plus FreeRTOS overhead.

**Fix:** keep the task handles from `xTaskCreatePinnedToCore()` and add
`uxTaskGetStackHighWaterMark()` per task to `AT+WDT?`, or a new `AT+TASKS?`. A
few lines, and the difference between diagnosing a field reboot and guessing.

## R18. [MEDIUM] No range validation on a loaded config blob

**Files:** `config_load_quiet()` in `src/config.cpp:55`

The only gates are the magic value and an exact size match. A blob that passes
both but holds an out-of-range field is accepted whole, and every CLI range check
is bypassed because those validate *input*, not what comes back out of NVS.

The worst case is `coins_required == 0`. `price_cents` is then 0, and the
division guards scattered through `app.cpp`, `session.cpp` and `display.cpp`
correctly return 0 blocks — but `ready` is `money_cents >= 0`, i.e. always true.
In `OP_AUTO_START` that becomes an endless loop: IDLE fires COIN_VALIDATE,
COIN_VALIDATE bills 0 blocks, denied beep + 600 ms hold, back to IDLE, repeat —
a machine that beeps at 300 Hz forever.

Note the individual division guards are all present and correct; it is the
*absence of a clamp on load* that lets a nonsense value in.

**Fix:** a `config_clamp(AppConfig*)` applying the same limits the CLI enforces,
called from `config_load()`. Cheap, and it turns a whole class of corrupt-blob
behaviour into a non-event.

## R19. [LOW] No `static_assert` on `sizeof(AppConfig)`

**Files:** `src/config.h`

The STM32 side guards its wire-format struct with `static_assert`s on `sizeof`
*and* field offsets (see the sibling repo's `display.cpp`). Here the only
protection is remembering to bump `APP_CONFIG_MAGIC`, and a same-size field
reorder passes both the size check in `config_load_quiet()` and the magic check —
silently misparsing every field.

Also worth knowing: the struct's mixed types mean `sizeof(AppConfig)` includes
padding bytes that get written to NVS as-is. Harmless given the magic check, but
it makes a stored blob non-comparable byte-for-byte.

**Fix:** add `static_assert(sizeof(AppConfig) == N)` and offset asserts on the
fields most likely to move, matching what the STM32 side already does.

## R20. [LOW] `coin_counter_init()`'s mutex creation is unchecked

**Files:** `coin_counter_init()` in `src/periph.cpp:216-218`

```c
void coin_counter_init() {
    s_coin_mutex = xSemaphoreCreateMutex();
}
```

No return check. Every accessor then does
`xSemaphoreTake(nullptr, portMAX_DELAY)`, which trips `configASSERT` and panics.
Only reachable on heap exhaustion at boot, so the practical risk is low — but
this is the money path, and `setup()` already checks and reports every
`xTaskCreatePinnedToCore()` result. Same treatment here would be consistent.

## R21. [LOW] `PIN_USER_LED` is written directly from `app.cpp`

**Files:** four `digitalWrite(PIN_USER_LED, ...)` calls in `src/app.cpp`

`periph.h` opens with "All direct GPIO / tone operations go here — nothing else
touches pins", and `app.cpp` is the one file that breaks it. There is a
`leds_set()` for the debug LED but no equivalent for the user LED.

**Fix:** add `user_led_set(bool)` to `periph.h` and route the four call sites
through it.

## R22. [MEDIUM — policy] `ENABLE_CLI` on a field unit is a free-vend backdoor

**Files:** `build_flags` in `platformio.ini`, the `loop()` comment in
`src/main.cpp`

Physical access to J7 (PROG) is the entire security boundary of this board — the
CLI has no authentication, and none is warranted for a wired serial header. But
that makes the build flag the control, and it is on by default:

- `AT+EEPROM_RESET` wipes the operator's pricing back to factory
- `AT+COINS_REQ=1` + `AT+RELAY_MS=3600000` is an hour of relay for one peso
- `AT+PRICE_PER_CREDIT` / `AT+COIN_POLARITY` change how money is counted

`main.cpp` already documents the trade-off in the other direction — dropping
`ENABLE_CLI` also removes the *only* way to set beep tones, display type, coin
polarity or factory-reset without reflashing, since none of those have an LCD
menu equivalent. That tension is real and is the reason this is a policy item
rather than a fix.

**Options, in rough order of preference:**

1. Ship field units without `ENABLE_CLI`, and give the LCD menu an entry for the
   settings that would otherwise become unreachable.
2. Keep the CLI but gate the destructive setters behind the JP10 strap or a
   physical jumper, leaving the read-only queries always available.
3. Accept it, and say so explicitly in `README.md` so it is a decision rather
   than an oversight.

---

## Changelog

| Date | Change |
| --- | --- |
| 2026-08-24 | First pass against `69260ad`. R1–R22 opened. |
