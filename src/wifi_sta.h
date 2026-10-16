#pragma once

#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// WiFi — the board's own way to the backend (2026-10-16)
//
// NAMED wifi_sta, NOT wifi. `src/wifi.h` shadows the Arduino core's own
// <WiFi.h> on a case-insensitive filesystem (Windows, macOS): the build puts
// `-Isrc` ahead of the framework include path, so `#include <WiFi.h>` in this
// very file resolved to itself and every WiFi symbol came back undeclared.
// `_sta` because the board is a station and never an access point.
//
// Until now the phone was the only gateway: a board's earnings reached the
// server when an operator stood next to it with the app open, and not before.
// WiFi makes the board a client in its own right — it checks in on its own
// schedule, so a machine that nobody visits still reports, and alerts can fire
// on something other than an operator's memory.
//
// BLE DOES NOT GO AWAY. WiFi is the second transport, not the replacement:
// provisioning happens over BLE, config and diagnostics still work over BLE,
// and a board whose WiFi is down is exactly as usable as it is today. The app
// prefers whichever is live.
//
// WHAT IT IS NOT
//
// NOT a way to update firmware unattended. The board never decides to flash
// itself; see src/ble_ota.h and the product decision of 2026-10-16 — a person
// presses the button.
//
// CREDENTIALS AND THE SECURITY BOUNDARY
//
// SSID, password and the backend device key arrive over BLE (WiFi Config,
// 6a40f00b) and live in their own NVS namespace. That means the WiFi password
// crosses an unencrypted BLE link, which is a deliberate, already-accepted
// trade in this product: physical proximity IS the security boundary, the same
// one that lets anyone within range write the config or flash the firmware.
// See docs/BLE_CONFIG_CONTRACT.md. It is worth knowing rather than discovering.
//
// The device key is a per-machine secret minted by the backend
// (POST /api/machines/:id/wifi-token). It is NOT the operator's login, it
// authorises exactly one machine's check-ins, and it is why check-in can run
// over plain HTTP without handing the fleet to anyone on the same network.
//
// THE RADIO IS SHARED, AND THAT HAS A COST
//
// WiFi and BLE share one antenna and one radio on this chip; the coexistence
// arbiter interleaves them. Expect BLE throughput to drop while WiFi is
// associated — which matters here specifically because firmware transfer over
// BLE was just taken from 2.6 to 26.5 KB/s and a WiFi-induced regression would
// look like that work coming undone. Measure, do not assume, and if it is bad
// the answer is to drop WiFi for the duration of a BLE OTA rather than to give
// the throughput back.
//
// NOTHING HERE MAY BLOCK THE VEND PATH
//
// Per src/wdt.h a blocking call does not stall this board, it reboots it. So:
// WiFi.begin() is fire-and-forget and the result is polled; the check-in POST
// runs on its own task, never in loop() and never in a GATT callback. The vend
// path must not be able to tell whether WiFi exists.
// =============================================================================

// Mirrors the app's WifiState (mobile/src/ble/constants.ts) and the contract's
// WiFi Status wire value. Do not renumber.
enum WifiConnState : uint8_t {
    WIFI_STATE_IDLE      = 0,   // no credentials, or deliberately forgotten
    WIFI_STATE_CONNECTING = 1,
    WIFI_STATE_CONNECTED  = 2,
    WIFI_STATE_FAILED     = 3,  // credentials present, association failed
};

// Call once from setup(), after config/identity are usable. Loads stored
// credentials and starts connecting if any exist. Cheap and non-blocking when
// the board has never been provisioned, which is every board today.
void wifi_init();

// Call from loop(). Drives the connect/retry state machine. Costs a status
// read and a millis() compare; does nothing at all when unprovisioned.
void wifi_service();

// Store credentials and begin connecting. Called from the WiFi Config
// characteristic (6a40f00b). Persists first, so a board that reboots mid-attempt
// comes back trying rather than forgetting.
//
// device_key may be empty: a board can be put on WiFi for local reachability
// before it has been claimed by an operator, and check-in simply stays off
// until a key arrives.
bool wifi_provision(const char* ssid, const char* password, const char* device_key);

// Disconnect and erase the stored credentials and device key. The explicit
// counterpart to provisioning — the radio does not forget just because a BLE
// session ended. Command op 0x07 (wifi_forget).
void wifi_forget();

WifiConnState wifi_state();

// IPv4 as a host-order u32, 0 unless connected. The contract puts this on the
// wire little-endian; see ble_wifi.cpp.
uint32_t wifi_ip();

// True when credentials are stored, regardless of whether the link is up.
// "Provisioned but not connected" is a different fault from "never set up",
// and an operator standing in front of a machine needs them told apart.
bool wifi_provisioned();

// Whether a backend device key is stored — i.e. whether check-ins can happen
// at all. Separate from provisioned(): a board can be on the network and still
// have nothing it is allowed to say.
bool wifi_has_device_key();

// Copies the stored device key out for the check-in client. Returns false when
// none is set. Buffer must be at least WIFI_DEVICE_KEY_BUF.
#define WIFI_DEVICE_KEY_BUF 65
bool wifi_device_key(char out[WIFI_DEVICE_KEY_BUF]);

// AT+WIFI? — state, SSID, IP, and why it is not connected if it is not.
void wifi_print_status(Stream& out);
