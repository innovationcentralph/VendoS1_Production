#pragma once
#include <Arduino.h>

// =============================================================================
// Watchdog — TPL5010 (U5), external
//
// This is the single biggest architectural difference between this firmware and
// the STM32 original, so it is worth reading before touching anything that
// calls watchdog_feed().
//
// -----------------------------------------------------------------------------
// What the STM32 did
// -----------------------------------------------------------------------------
// The STM32 used its INTERNAL independent watchdog (IWDG) with a ~25 s timeout,
// started by watchdog_init() and kicked by watchdog_feed() calls sprinkled
// through the app task, the session countdown, the config menu and the beep
// helper. Because the timeout was 25 s and every one of those loops ran far
// faster than that, the effect was: if the app task itself wedged, feeding
// stopped and the MCU reset. The watchdog supervised the APPLICATION, not just
// the chip.
//
// -----------------------------------------------------------------------------
// What the S1 has instead
// -----------------------------------------------------------------------------
// A TPL5010 sitting outside the ESP32 entirely. Its nRST output drives the
// ESP_EN net, so an unpetted watchdog HARD-RESETS the whole module and nothing
// in software can mask it. Two consequences:
//
//   1. The timeout is FIXED IN HARDWARE and short. It is programmed by R34
//      40.2K (with R33 10K + solder jumper JP14 as a stuff option) and is not
//      readable by firmware — the TPL5010 wires DONE but leaves WAKE
//      unconnected, so there is no signal telling us when the window opens. We
//      can only pulse DONE more often than the timeout. The range bottoms out
//      around 100 ms, hence WDT_PET_MS below.
//
//   2. A 100 ms budget cannot be met by feed calls sprinkled through
//      application code. The app deliberately blocks for far longer than that
//      in several places the STM32 was fine with — the 2 s boot splash, the 3 s
//      "COMPLETE!" hold, the 600 ms denied message, a multi-beep pattern. Under
//      a 25 s IWDG those were free; under a ~100 ms external watchdog every one
//      of them would reset the board mid-vend.
//
// So petting moved to a dedicated high-priority task (wdt_task_run) that pulses
// DONE every 100 ms unconditionally, and watchdog_feed() became a LIVENESS
// CHECK-IN rather than the thing that actually pets.
//
// -----------------------------------------------------------------------------
// PENDING — this is a deliberate, temporary simplification
// -----------------------------------------------------------------------------
// Because the pet is unconditional, this watchdog currently catches total system
// death (crash, panic, brownout, core lockup) but NOT an application-level hang:
// if the app task wedged while the wdt task kept running, the board would sit
// there petting happily forever. The STM32 caught that case. Restoring it is
// the pending item — see docs/PENDING.md, item 1.
//
// The scaffolding for it is already here and already wired up at every STM32
// feed site: watchdog_feed() stamps a per-caller timestamp, and
// watchdog_liveness_stale_ms() reports the worst-offending one. Enforcing it is
// a single `if` inside wdt_task_run(). It is left OFF for now because choosing
// the deadline needs bench numbers we do not have yet (see PENDING item 2 —
// the real TPL5010 timeout still needs measuring with AT+WDT=0 on the
// Vendo_S1_TestCode harness), and a deadline guessed too tight would reboot the
// board mid-session, which is strictly worse than not supervising at all.
// =============================================================================

// Pet interval. Chosen to stay inside even the shortest plausible TPL5010
// window rather than to be efficient — see PENDING item 2.
#define WDT_PET_MS 100

// Liveness check-in sources. One slot each so a stall can be attributed to the
// task that actually stopped, instead of just "something stopped".
typedef enum : uint8_t {
    WDT_SRC_APP     = 0,   // app_task_run state machine
    WDT_SRC_SESSION = 1,   // session_run countdown loop
    WDT_SRC_MENU    = 2,   // config_menu_run
    WDT_SRC_COUNT
} WdtSource;

// Drive IO2 low and pulse DONE once. Call this as the FIRST thing in setup(),
// before Serial: if the programmed timeout is at the short end of the TPL5010
// range, even bringing up the UART could outlast it. Also latches the reset
// reason for wdt_print_boot_report().
void wdt_begin();

// FreeRTOS task body — pulses DONE every WDT_PET_MS. Create it FIRST in setup(),
// at the highest priority of any app task, so nothing else can starve it.
void wdt_task_run(void* arg);

// Pulse DONE right now, regardless of the interval. The wdt task makes this
// unnecessary in normal operation; it exists for the one place that runs before
// the task is scheduled (LCD power-on init, whose datasheet waits total ~65 ms).
void wdt_pet();

// -----------------------------------------------------------------------------
// Liveness check-in. This is what the STM32 watchdog_feed() call sites became.
//
// Deliberately keeps the STM32 name and signature so every call site ports
// across verbatim and the two codebases stay diffable. It no longer pets the
// hardware — it records that `src` is still making progress.
// -----------------------------------------------------------------------------
void watchdog_feed(WdtSource src);

// Kept for STM32 source compatibility — feeds as WDT_SRC_APP.
inline void watchdog_feed() { watchdog_feed(WDT_SRC_APP); }

// Milliseconds since the most-stale registered check-in, ignoring sources that
// have never checked in at all (a source that has not run yet is not stalled).
// This is the number the PENDING enforcement will threshold on.
uint32_t watchdog_liveness_stale_ms();

// Report why the board last rebooted. Worth printing at every boot: on this
// board a watchdog reset and a press of SW1 are indistinguishable (both land on
// ESP_RST_EXT, since nRST and the reset button share the ESP_EN net), and
// telling either of those apart from a panic or a brownout is the first
// question any field fault raises.
void wdt_print_boot_report(Stream& out);

void wdt_print_status(Stream& out);

uint32_t wdt_pet_count();
