#pragma once
#include <Arduino.h>

// =============================================================================
// Verbose debug logging — compiled out entirely unless VERBOSE_LOG is defined
// (see platformio.ini build_flags). Use for chatty, dev-only traces (per button
// press, per session tick, per coin pulse); leave boot messages, CLI command
// responses, and config dumps as plain Serial calls since those are real
// functionality, not debug noise.
//
// Note the log stream: the STM32 firmware logged to Serial1 (PA9/PA10). On the
// S1 the equivalent header is J7 (PROG), which is UART0 — plain `Serial`. Every
// Serial1 in the STM32 sources maps to Serial here and nothing else changes.
// =============================================================================
#ifdef VERBOSE_LOG
    #define DBG(...)   Serial.print(__VA_ARGS__)
    #define DBGLN(...) Serial.println(__VA_ARGS__)
#else
    #define DBG(...)   ((void)0)
    #define DBGLN(...) ((void)0)
#endif
