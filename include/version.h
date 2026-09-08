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
