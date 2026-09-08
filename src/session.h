#pragma once
#include "config.h"

// =============================================================================
// Timer Mode — one vend cycle
//
// Rules enforced here:
//   - Relay ON for blocks * cfg->relay_on_ms (loaded from NVS)
//   - Start beep before relay ON; end beep after relay OFF
//   - Accumulation and Sensor Stop are composable with any operation_mode
// =============================================================================

// Run one vend cycle from start to end.
// The caller has already consumed exactly `blocks * cfg->coins_required * 100`
// centavos from the running coin value — any leftover partial-credit cents
// are left banked, carried over rather than discarded.
// `blocks` is the number of satisfied credits (coins_required is the peso
// price of ONE credit, not a raw pulse count).
// Total relay ON time = blocks * cfg->relay_on_ms (relay_on_ms is per credit).
// Blocks (FreeRTOS-sense) until the session expires, then returns.
//
// `extra_credits_out` (optional): when accumulation_enabled AND the caller is
// running under (OP_PRESS_TO_START or OP_PAUSE_RESUME) + press_to_start_per_credit
// gating, mid-session coins must NOT stretch the block currently dispensing —
// they instead count as additional gated credits owed later. In that
// combination, session_run() bills the coins as usual but adds the resulting
// block count to *extra_credits_out instead of extending its own deadline;
// the caller folds that into remaining_credits. Ignored (accumulation extends
// the running deadline as normal) for every other mode/flag combination, or
// when left null.
void session_run(const AppConfig* cfg, uint32_t blocks, uint32_t* extra_credits_out = nullptr);
