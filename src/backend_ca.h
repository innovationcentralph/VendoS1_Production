#pragma once

// =============================================================================
// Root certificate for the backend's TLS, so the board can tell the real
// server from anything else on the network.
//
// The board's check-in carries its device key, which is the credential that
// authorises one machine's earnings reports. Without server authentication
// anything on the same WiFi could present itself as the backend and collect
// that key; WiFiClientSecure::setInsecure() would have made the HTTPS in the
// URL decorative.
//
// ISRG Root X2, the root the server's chain actually terminates at (fetched
// from app-api.vendolabsph.com on 2026-10-16, NOT typed from memory - the
// chain roots at X2, and the far better known X1 would simply have failed).
// Valid until 2040-09-18.
//
// WHEN THIS NEEDS CHANGING: if the backend ever moves off Let's Encrypt, or
// Let's Encrypt retires this root. The symptom is every check-in failing TLS
// at once while BLE carries on working perfectly - look here first.
// =============================================================================

static const char BACKEND_ROOT_CA[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQswCQYDVQQGEwJV\n"
    "UzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElT\n"
    "UkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVT\n"
    "MSkwJwYDVQQKEyBJbnRlcm5ldCBTZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNS\n"
    "RyBSb290IFgyMHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0H\n"
    "ttwW+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9ItgKbppb\n"
    "d9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0TAQH/BAUwAwEB/zAdBgNV\n"
    "HQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZIzj0EAwMDaAAwZQIwe3lORlCEwkSHRhtF\n"
    "cP9Ymd70/aTSVaYgLXTWNLxBo1BfASdWtL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5\n"
    "U6VR5CmD1/iQMVtCnwr1/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";