#pragma once

#include <NimBLEDevice.h>

// =============================================================================
// WiFi provisioning over BLE — Config (6a40f00b) and Status (6a40f00c)
//
// The two halves of putting a board on a network from the app:
//
//   Config  (Write)       ssid_len:u8, ssid, password_len:u8, password,
//                         device_key_len:u8, device_key
//   Status  (Read/Notify) state:u8, ip:u32
//
// Wire layout is the app team's — VendoLabs PH/Wiki/Board-Firmware-Contract.md.
//
// Status is NOTIFIED on every state change, because the app's provisioning
// screen is a form someone is watching: they typed a password and want to know
// within a second or two whether it was right. A read-only characteristic
// would leave the app polling for something that takes up to 20 s to fail.
//
// The write handler is a mailbox, like Command and OTA Control: WiFi.begin()
// and an NVS write have no business running on the NimBLE host task, which is
// the same task that owes the phone an ATT response for this very write.
// =============================================================================

void ble_wifi_register(NimBLEService* service);

// Call from loop(). Performs a queued provisioning request and pushes a Status
// notify whenever the connection state has moved. Does nothing otherwise.
void ble_wifi_service();
