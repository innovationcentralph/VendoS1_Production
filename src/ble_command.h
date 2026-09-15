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

// Op codes — mobile/src/ble/constants.ts CommandOp. Do not invent new ones:
// 0x01-0x07 are all allocated, and `reboot` (APP_BLE_PLAN B6/A4) deliberately
// has no code yet because assigning one is the app team's call.
#define CMD_OP_SYNC_ACK      0x01
#define CMD_OP_CLEAR_ERRORS  0x02
#define CMD_OP_IDENTIFY      0x03
#define CMD_OP_TEST_RELAY    0x04
#define CMD_OP_TEST_BUZZER   0x05
#define CMD_OP_TEST_LED      0x06
#define CMD_OP_WIFI_FORGET   0x07

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
