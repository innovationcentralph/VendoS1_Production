#pragma once

// ============================================================================
// Diagnostics characteristic — 6a40f006, Read + Notify
//
// Wire format and error codes live in src/diag.h; this file is only the GATT
// surface. Variable length: 11 header bytes + one byte per raised fault.
//
// The fault list is a MAILBOX, not a snapshot — the app reads it, uploads each
// code to the backend, then sends the Command op clear_errors to empty it.
// See src/diag.h for why that shapes retention.
//
// ⚠️ clear_errors arrives on f007 Command, which is NOT built. Until it is,
// nothing over BLE can empty this list, so the same faults re-upload on every
// connect. AT+DIAG_CLEAR is the bench workaround. f007 is required for the
// f001 bundle regardless — the app's sync flow calls sendCommand(syncAck)
// unconditionally and without a catch.
// ============================================================================

#include <NimBLEDevice.h>

// Creates the characteristic on an existing service. Call from
// ble_config_init() before service->start().
void ble_diagnostics_register(NimBLEService* service);

// Notifies subscribers when a fault is raised or cleared. Call from loop().
void ble_diagnostics_service();
