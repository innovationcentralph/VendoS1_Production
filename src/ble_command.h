#pragma once

// ============================================================================
// Command characteristic — 6a40f007, Write only, 5 bytes: op:u8 + param:u32 LE
//
// UUID and op codes from the app's allocation table and encodeCommand(); see
// docs/BLE_CONFIG_CONTRACT.md. ESP32/S1 only — no STM32 counterpart.
//
// THIS ONE IS NOT OPTIONAL, WHICH IS WHY IT EXISTS AT ALL
//
// BleConnectionContext.tsx calls acknowledgeSync() -> sendCommand(syncAck)
// UNCONDITIONALLY, with no .catch(), two lines after a readWifiStatus() that
// does have one — they made WiFi non-fatal and deliberately did not do the same
// here. Without this characteristic the connect throws AFTER the backend upload
// has already succeeded: the data lands server-side, the app reports a failure,
// and the board never learns what was synced. It also carries clear_errors,
// without which f006's faults re-upload on every connect forever.
//
// ---------------------------------------------------------------------------
// NOTHING IS EXECUTED IN THE GATT CALLBACK. IT IS A MAILBOX.
// ---------------------------------------------------------------------------
//
// The write callback runs on the NimBLE host task, and not one of the physical
// ops may run there:
//
//   * buzzer_beep_*() blocks on vTaskDelay for hundreds of milliseconds. Doing
//     that inside a GATT callback stalls the whole BLE stack, including the
//     connection that is waiting for the ATT response to this very write.
//   * the relay and the LEDs belong to the vend state machine. Driving them
//     from a second task is two owners on one GPIO, which is how a relay ends
//     up latched on with nobody responsible for turning it off.
//
// So the callback validates, queues, and returns; ble_command_service() drains
// the queue from loop(). Same shape display.cpp uses for the LCD, and for the
// same reason.
//
// ---------------------------------------------------------------------------
// EVERY PHYSICAL OP IS GATED ON "IS THIS BOARD VENDING?"
// ---------------------------------------------------------------------------
//
// A relay test during a paid session is a customer complaint, and a test beep
// mid-vend reads as a fault. app_state_is_idle() is the gate; a rejected op is
// dropped and logged, never deferred — replaying it minutes later, after the
// operator has stopped watching, is worse than not running it.
//
// ⚠️ A rejection is INVISIBLE TO THE APP. This characteristic is write-only and
// the contract gives it no status channel, so the ATT write succeeds either
// way. That is inherent to the protocol, not an implementation shortcut. If the
// app ever needs to know, it needs a field from their side (see D20).
//
// ---------------------------------------------------------------------------
// WHAT sync_ack MUST NOT DO
// ---------------------------------------------------------------------------
//
// It carries the backend's idea of the last synced seq, and it is recorded for
// diagnostics ONLY. It must never be written into counters_set_last_seq(): that
// value comes from the server, it can legitimately be LOWER than the board's
// own (a partial sync, a restored backup, a second phone), and writing it back
// would rewind the sequence numbers that make delta sync idempotent — the exact
// corruption src/eventlog.h is built to prevent.
//
// The log is a rolling window that expires by age, not by acknowledgement, so
// there is nothing to free on ack either. Logging it is the whole job.
// ============================================================================

#include <NimBLEDevice.h>

// Op codes — mobile/src/ble/constants.ts CommandOp. 0x01-0x07 are the app's,
// and none of them may be repurposed: the app has them compiled in, so a
// mismatch is a silent misparse, not an error.
#define CMD_OP_SYNC_ACK      0x01
#define CMD_OP_CLEAR_ERRORS  0x02
#define CMD_OP_IDENTIFY      0x03
#define CMD_OP_TEST_RELAY    0x04
#define CMD_OP_TEST_BUZZER   0x05
#define CMD_OP_TEST_LED      0x06   // D7, the blue DEBUG led on IO15
#define CMD_OP_WIFI_FORGET   0x07

// ⚠️ PROVISIONAL BLOCK 0x08-0x0B, NOT YET RATIFIED BY THE APP TEAM.
//
// These three are ours, claimed ahead of an allocation (user's decision,
// 2026-09-17). 0x08 is the next free code and therefore also the natural code
// for `reboot`, which APP_BLE_PLAN A4 leaves open precisely because assigning
// it is the app team's call — and the exposure grows with each code taken: a
// collision on 0x09/0x0A would make the app silently enter test mode, i.e. take
// a machine out of service, rather than merely blink a lamp. Get the block
// ratified, or moved, before any board ships with it. See A13.
//
// Distinct from CMD_OP_TEST_LED on purpose: that one drives D7 (the blue debug
// LED on IO15, which no customer ever sees), while this drives the J1
// illuminated button lamp on IO23 — the only LED on the machine a customer
// looks at, and so the only one whose failure is a field complaint.
#define CMD_OP_TEST_USER_LED 0x08

// Test mode, param 1 = enter / 0 = leave. One op with the param rather than two
// codes: the wire frame already carries param:u32 and every op but sync_ack
// ignores it, so spending a second scarce code on a boolean would be waste.
// Entering is refused unless the board is idle - it must never abandon a paid
// session. Leaving is always accepted. See the TEST MODE block in app.h for the
// three exits and why a board stuck in this state is the hazard to design
// against. ALSO PROVISIONAL - see A13.
#define CMD_OP_TEST_MODE     0x09

// Coin slot during test mode, param 1 = accept / 0 = inhibit. Refused outside
// test mode: everywhere else the coin slot belongs to the vend state machine
// (session.cpp inhibits it for the duration of a session, the config menu for
// the duration of the menu), and a second owner toggling IO12 would either take
// money during a session that cannot bill it or inhibit one that should.
// Enabling restarts the pulse count from zero. ALSO PROVISIONAL - see A13.
#define CMD_OP_TEST_COIN_SLOT 0x0A

// User button (START, J1 pin 1 / IO18) press counter: param 0 resets it to
// zero. The counterpart to test_user_led on J1 pin 2 - verifying that harness
// means checking the lamp and the button together.
//
// THERE IS NOTHING TO "RUN" HERE, AND THAT IS THE POINT. A button is an input;
// the board cannot press it. So the count is live for the whole of test mode
// with no command at all, and this op only re-zeroes it so a wizard step can
// say "press it three times" and read exactly three. If the app team reassigns
// this code, the button test still works - only the reset is lost. Deliberate:
// see A13, and do not build a test whose only entry point is an unratified
// code. ALSO PROVISIONAL.
#define CMD_OP_TEST_BUTTON   0x0B

// How long testRelay energises the relay. Long enough for a technician to hear
// the click and see the load twitch, short enough that it cannot be mistaken
// for a vend or dispense anything worth money.
#define CMD_RELAY_TEST_MS  500

// Creates the characteristic on an existing service. Call from
// ble_config_init() before service->start().
void ble_command_register(NimBLEService* service);

// Executes at most one queued command. Call from loop(). Blocks for the length
// of whatever it runs (a beep pattern, the relay test) — which is safe there:
// the watchdog is petted by its own task, the vend path is a different task,
// and the CLI is the only thing that waits.
void ble_command_service();

// Last seq the app said the backend has, and whether one was ever received.
// Diagnostics only — see the header note above. Reported by AT+SYNC?.
uint32_t ble_command_last_acked_seq();
bool     ble_command_ever_acked();
