#pragma once

#include <NimBLEDevice.h>

// =============================================================================
// BLE OTA firmware update — Control (6a40f008), Data (6a40f009), Status (6a40f00a)
//
// Receives a firmware image over BLE and boots into it. The contract, op codes
// and wire layout are the app team's: VendoLabs PH/Wiki/Board-Firmware-Contract.md,
// "OTA firmware update". Ported from firmware/esp32-bringup, which has run this
// flow against real hardware; the differences from that version are listed at
// the bottom of this comment and each is a property of THIS board.
//
// FLOW
//
//   app writes ota_begin {size, crc32}  -> board erases the inactive slot
//   app writes chunks to Data            -> board writes them through to flash
//   app writes ota_commit                -> board checks size + CRC32, then
//                                           finalises the image and reboots
//
// Progress is reported by notifying Status; there is no per-chunk ack, because
// a round trip per chunk at this link's throughput would roughly halve it.
//
// NOTHING IS COMMITTED UNTIL THE WHOLE IMAGE IS VERIFIED. A dropped connection,
// a failed check or an abort leaves the running firmware untouched — the image
// lands in the OTHER slot, and the boot partition is only switched by
// Update.end(true). There is no resume: the app starts again from ota_begin.
//
// HOW THIS DIFFERS FROM THE BRING-UP IMPLEMENTATION, AND WHY
//
// 1. The Control characteristic is a MAILBOX, not an executor — the same rule
//    as Command (6a40f007), for a sharper reason here. Update.begin() erases
//    the whole destination slot (1.875 MB), which takes on the order of a
//    second, and Update.end(true) verifies the image. Doing either inside a
//    GATT callback stalls the NimBLE host task — including the ATT response
//    the phone is waiting on — long enough to risk the connection dropping
//    mid-update. So Control records the request and loop() performs it.
//
//    Data is the exception and stays inline: a chunk is a few hundred bytes,
//    the write is fast, and there is nowhere to buffer a 1.8 MB image anyway.
//
// 2. An OTA is refused unless the board is IDLE. Not because it drives an
//    output, but because it REBOOTS: doing that mid-session abandons a vend a
//    customer has already paid for, and coins counted but not yet banked are
//    lost with it. Same gate and same reason as reset_earnings.
//
// 3. periph_reset_safe() runs before the restart. On this board the strapping
//    pins and the relay need putting back by firmware on a warm restart —
//    external resistors only cover a cold power-on. See src/periph.h.
//
// 4. The image arrives on trial. src/ota_guard.h holds it in PENDING_VERIFY
//    until it proves it can still be reached over BLE, and rolls back if not.
//    That is what makes a bad image survivable without a site visit.
// =============================================================================

// Register the three characteristics. Called from ble_config_init(), like every
// other characteristic module.
void ble_ota_register(NimBLEService* service);

// Call from loop(). Performs whatever the Control characteristic queued (erase,
// commit, abort) and pushes a Status notify when anything changes. Does nothing
// when no update is in flight.
void ble_ota_service();

// AT+OTA? prints this alongside the slot/rollback state from ota_guard.
//
// There is deliberately no ble_ota_in_progress() accessor. One was written and
// removed unused: the only caller it was meant for was the vend path, and
// app.cpp is kept diffable against the STM32 (see CLAUDE.md), which has no OTA
// — so an OTA test does not belong in its state machine. The race it would have
// guarded, a session starting during a transfer, is handled at the other end
// instead: ota_commit re-checks idle and defers rather than rebooting mid-vend.
void ble_ota_print_status(Stream& out);
