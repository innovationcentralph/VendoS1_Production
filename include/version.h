#pragma once

// Firmware version — bump on every release (semver: MAJOR.MINOR.PATCH).
// MAJOR: breaking changes (config layout, CLI command removal/behavior change)
// MINOR: new features, backward compatible
// PATCH: bug fixes, no new behavior
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 0

// The -esp32 suffix is deliberate. This firmware is at behaviour parity with the
// STM32 VendoBoard production firmware 1.0.0 (branch main, commit ee8e253), and
// the two are field-swappable from an operator standpoint — but they are
// different binaries for different boards, and a version string that could not
// tell them apart would be worse than useless on a support call. Bump the
// numeric part in step with the STM32 side whenever behaviour is ported across.
#define FW_VERSION_STRING "1.0.0-esp32"

// ---------------------------------------------------------------------------
// BUILD ID (2026-10-16)
// ---------------------------------------------------------------------------
//
// The semver above is bumped by hand, which means it does not change between
// releases — it sat at "1.0.0-esp32" across every behavioural change from the
// first commit (2026-09-08) to this one. That was survivable while the only way
// to flash a board was to stand in front of it with a cable. OTA removes that:
// the server has to decide whether a board is out of date, and a version that
// cannot distinguish two builds cannot answer it.
//
// So the commit sha is stamped in at build time by scripts/build_id.py, with
// "-dirty" appended when the working tree had uncommitted changes. Use:
//
//   FW_VERSION_STRING   short, stable, human — the LCD's 16-char row (display.cpp)
//                       and anywhere a semver comparison is what is wanted
//   FW_VERSION_FULL     the one that identifies a specific binary — BLE Device
//                       Info (f001), AT+VERSION?, the boot banner
//
// The fallback matters: a build without the hook (plain `gcc`, an editor's
// index, a tarball with no .git) still compiles, and says "unknown" rather than
// claiming an identity it does not have.
#ifndef FW_BUILD_ID
#define FW_BUILD_ID "unknown"
#endif

#ifndef FW_BUILD_EPOCH
#define FW_BUILD_EPOCH 0
#endif

// e.g. "1.0.0-esp32+a1b2c3d" or "1.0.0-esp32+a1b2c3d-dirty".
//
// '+' is semver's build-metadata separator, and semver says build metadata is
// ignored when comparing precedence — which is exactly right here. The server
// compares the part before '+' to decide newer-or-older, and uses the part
// after it to tell two builds of the same version apart. A client that does not
// understand the suffix still parses the version.
#define FW_VERSION_FULL FW_VERSION_STRING "+" FW_BUILD_ID
