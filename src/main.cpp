#include <Arduino.h>
#include <esp_system.h>
#include "app.h"
#include "periph.h"
#include "config.h"
#include "display.h"
#include "wdt.h"
#include "version.h"
#ifdef ENABLE_CLI
#include "cli.h"
#endif
#ifdef ENABLE_BLE
#include "ble_config.h"
#endif

// =============================================================================
// Task layout
//
// The STM32 build called vTaskStartScheduler() at the end of setup(). On
// Arduino-ESP32 the scheduler is ALREADY RUNNING before setup() is entered —
// setup() itself is a FreeRTOS task (loopTask). So tasks created here start
// immediately rather than when setup() returns, and there is no
// vTaskStartScheduler() call to port.
//
// Two consequences worth knowing:
//   * Stack sizes below are in BYTES, not words. The STM32 numbers (256/512/768)
//     were words; naively copying them here would give a quarter of the stack
//     and overflow on the first snprintf.
//   * Creation ORDER now matters for real. The wdt task must exist before
//     anything that can block, because the TPL5010 window is ~100 ms.
//
// All app tasks are pinned to core 1 (APP_CPU), leaving core 0 free for the WiFi
// and BLE stacks this board is eventually meant to use. Pinning them together
// also means a task that spins without yielding starves core 1 idle, which trips
// the ESP32 own task watchdog — free supervision we would lose by spreading them.
// =============================================================================
#define TASK_CORE            1

#define TASK_PRIO_WDT        6   // above everything — nothing may starve the pet
#define TASK_PRIO_COIN       4   // above the app task: this one counts money
#define TASK_PRIO_APP        3
#define TASK_PRIO_DISPLAY    2

#define TASK_STACK_WDT    2048
#define TASK_STACK_COIN   2048
#define TASK_STACK_APP    4096
#define TASK_STACK_DISP   4096   // sized for snprintf + Wire + FreeRTOS overhead

// =============================================================================
// Safety on the way down
//
// The STM32 defined vApplicationStackOverflowHook and vApplicationMallocFailedHook
// to drop the relay with a direct register write (GPIOA->BRR) and then reset. On
// ESP-IDF those hooks are already defined by the framework as strong symbols and
// cannot be overridden — a stack overflow or failed allocation panics and reboots
// on its own.
//
// The relay is still safe in that case, by the same mechanism the STM32 relied on
// after ITS reset: on an ESP32 reset every GPIO reverts to a high-impedance input,
// and Q2 gate has R19 10K to ground, so the coil de-energises with no firmware
// involvement at all. What the shutdown handler below adds is covering the CLEAN
// restart paths (AT+RESET, esp_restart from anywhere) a fraction earlier, before
// the pin state actually changes.
//
// It is deliberately a bare digitalWrite and nothing else: this runs in a
// shutdown context where allocating, logging or taking a lock could hang.
// =============================================================================
static void relay_off_on_shutdown() {
    digitalWrite(PIN_RELAY, LOW);
}

// =============================================================================
// setup
// =============================================================================
void setup() {
    // ---- FIRST, before anything else, including Serial. ----
    // If the TPL5010 timeout is at the short end of its range, even bringing up
    // the UART could outlast it. wdt_begin() also drives IO2 low before making it
    // an output, which matters because IO2 is a strapping pin. See src/wdt.h.
    wdt_begin();

    // Start petting before any of the boot printing below. config_dump_eeprom()
    // alone pushes several hundred bytes at 115200, which is well past 100 ms.
    xTaskCreatePinnedToCore(wdt_task_run, "wdt", TASK_STACK_WDT, nullptr,
                            TASK_PRIO_WDT, nullptr, TASK_CORE);

    // Serial maps to UART0 on IO1/IO3 — the J7 PROG header. This is the
    // equivalent of the STM32 Serial1 on PA9/PA10.
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("=== Vendo S1 Production boot ===");
    Serial.print("Firmware version: "); Serial.println(FW_VERSION_STRING);
    Serial.println("Port of VendoBoard STM32 production firmware (main @ ee8e253)");

    // Why the board last rebooted. On this hardware a watchdog reset and a press
    // of SW1 are indistinguishable (shared ESP_EN net), but telling either apart
    // from a panic or a brownout is the first question any field fault raises.
    wdt_print_boot_report(Serial);

    Serial.println("Config: BTN1+BTN2 long press = enter/save, START tap = page, START long = cancel");
#ifdef ENABLE_CLI
    Serial.println("Type AT+HELP? for commands");
#endif

    // Bring the relay and both strapping-pin outputs to a safe state before the
    // app task gets there. periph_init() runs again inside APP_STATE_INIT (as it
    // did on the STM32), but that is several hundred ms of boot printing away,
    // and the relay should not be left to chance for that long.
    periph_init();
    esp_register_shutdown_handler(relay_off_on_shutdown);

    // Show exactly what is stored before the app task may rewrite it.
    config_dump_eeprom(Serial);

#ifdef ENABLE_BLE
    // BLE runs entirely on the Bluedroid stack's own tasks (pinned to core 0
    // by the framework), not on anything created here — see the task-layout
    // comment above for why core 0 was left free for this. Safe to start
    // before the app task exists: it only ever touches NVS via config.cpp's
    // own API, the same as the CLI does.
    ble_config_init();
#endif

    // Coin counter mutex must be created before any task that reads the counter.
    coin_counter_init();

    // Display queue must be created before any task that posts to it starts.
    display_early_init();

    // Coin counting task — polls PIN_COIN_IN with debounce; runs independently.
    if (xTaskCreatePinnedToCore(coin_counter_task_run, "coin", TASK_STACK_COIN, nullptr,
                                TASK_PRIO_COIN, nullptr, TASK_CORE) != pdPASS)
        Serial.println("WARN: coin counter task creation failed");

    // Display task — sole owner of I2C/LCD; decouples the LCD from relay timing.
    if (xTaskCreatePinnedToCore(display_task_run, "disp", TASK_STACK_DISP, nullptr,
                                TASK_PRIO_DISPLAY, nullptr, TASK_CORE) != pdPASS)
        Serial.println("WARN: display task creation failed");

    // Main application state machine task.
    if (xTaskCreatePinnedToCore(app_task_run, "app", TASK_STACK_APP, nullptr,
                                TASK_PRIO_APP, nullptr, TASK_CORE) != pdPASS) {
        Serial.println("FATAL: app task creation failed");
        // Do NOT spin here the way the STM32 did. With no app task there is
        // nothing to vend, and a bare for(;;) would sit petting the watchdog
        // forever, presenting as a dead-but-powered machine. Reboot instead and
        // let it either recover or fail loudly and repeatedly.
        Serial.flush();
        delay(100);
        esp_restart();
    }

    Serial.print("Free heap after task creation: ");
    Serial.println((unsigned)esp_get_free_heap_size());
    Serial.println("Running.");
    Serial.flush();
}

// =============================================================================
// loop — the AT command CLI
//
// The STM32 ran this as its own FreeRTOS task (TaskCli) because its loop() was
// dead code once the scheduler took over. Here loopTask is a real task that
// already exists with an 8 KB stack, and polling a UART at 10 ms is exactly what
// it is good for — so the CLI lives here instead of paying for a second task.
// Same behaviour, one less stack.
//
// ENABLE_CLI is on by default (dev builds). Omit it for a production build to
// drop the entire AT+ command parser — note this also removes the only way to
// set beep tone (AT+BEEP_START/END/OFF_MS), display type (AT+DISPLAY_TYPE),
// coin pulse polarity (AT+COIN_POLARITY), or factory-reset the config
// (AT+EEPROM_RESET) without reflashing, since none of those have an LCD
// equivalent.
// =============================================================================
void loop() {
#ifdef ENABLE_CLI
    while (Serial.available() > 0) {
        cli_feed_char((char)Serial.read(), Serial);
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
}
