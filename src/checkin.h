#pragma once

#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// Backend check-in — the board reporting for itself (2026-10-16)
//
// Until now every peso reached the server by way of an operator's phone: a
// machine nobody visited was a machine nobody could see. This posts the same
// sessions the phone would relay, straight to the backend over WiFi, on the
// board's own schedule.
//
// It is the half that makes WiFi mean anything to the app. getConnectionStatus()
// already prefers WiFi over Bluetooth on every screen, but it decides a board
// is reachable from Machine.lastWifiCheckinAt — a timestamp only this can set.
// Without it a board can sit happily on a network while every screen still
// says "Connected via Bluetooth".
//
// SAME ENDPOINT, SAME SHAPE AS THE PHONE
//
//   POST {BACKEND}/api/board/{deviceId}/checkin
//   X-Device-Key: <the per-machine key minted at provisioning time>
//   {"firmwareVersion":..., "heartbeat":{...}, "sessions":[...]}
//
// The server runs it through the same runSync() as the phone's relay, so the
// two paths cannot drift. Sessions are keyed on (machine, seq) server-side and
// are therefore idempotent: re-sending one is wasteful, never wrong.
//
// WHY IT HAS ITS OWN TASK
//
// A TLS handshake and an HTTP round trip take seconds on a bad network. Per
// src/wdt.h a blocking call in the vend path does not stall this board, it
// REBOOTS it — so this cannot live in loop() the way the lighter services do.
// It runs at a priority below the display task, so it can never delay a coin,
// a relay or a screen, and it spends almost all of its life blocked on a
// socket.
//
// WHAT IT DOES NOT DO
//
// It does not act on the response. The backend replies with a pending config
// and, if the operator asked for one, a firmware URL — both are READ AND
// IGNORED for now. Applying config needs a path into the running app task that
// does not exist yet, and fetching firmware unattended is explicitly not what
// this product does (2026-10-16: a person presses the button).
// =============================================================================

// Creates the task. Call once from setup(), after wifi_init() and identity.
// Does nothing useful until the board has WiFi, a device key and a serial —
// it simply sleeps and re-checks, so an unprovisioned board costs one task.
void checkin_init();

// AT+CHECKIN? — when it last succeeded, what it last sent, and why it is not
// running if it is not.
void checkin_print_status(Stream& out);
