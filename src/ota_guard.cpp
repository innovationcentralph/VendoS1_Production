#include "ota_guard.h"

#include "periph.h"
#include "wdt.h"
#include "version.h"
#ifdef ENABLE_BLE
#include "ble_config.h"
#endif

#include <esp_ota_ops.h>
#include <esp_system.h>

// How stale the app task's liveness check-in may be and still count as healthy.
// The app task checks in every loop of its state machine; 3 s is far longer
// than it ever takes, so this trips only on a genuinely stuck app.
#define LIVENESS_OK_MS  3000

static bool     s_pending      = false;   // this boot is an unconfirmed OTA image
static bool     s_settled      = false;   // ...and we have since decided its fate
static uint32_t s_start_ms     = 0;       // when the self-test began
static uint32_t s_healthy_since = 0;      // when the criteria last became true (0 = not now)

// -----------------------------------------------------------------------------
// The override that makes any of this work.
//
// Arduino's weak definition returns false, which is what lets initArduino() mark
// a fresh OTA image valid before setup() runs. Returning true keeps the image in
// PENDING_VERIFY so the code below decides instead.
//
// extern "C" and this exact signature are load-bearing: a C++-mangled or
// differently-typed symbol does not override the weak one, it just sits beside
// it, and the failure is SILENT — rollback goes back to being inert with
// nothing to show for it. If this ever stops working, check the symbol first.
// -----------------------------------------------------------------------------
extern "C" bool verifyRollbackLater() {
    return true;
}

// Is the board reachable and running right now?
static bool self_test_ok() {
#ifdef ENABLE_BLE
    // The way back in. Without this an update can never be undone over the air.
    if (!ble_config_is_reachable()) return false;
#endif
    // Advertising while the vend path is wedged is not "working".
    if (watchdog_liveness_stale_ms() > LIVENESS_OK_MS) return false;
    return true;
}

static void confirm() {
    s_settled = true;
    s_pending = false;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        Serial.println("OTA:    image CONFIRMED — rollback cancelled, this build is now permanent");
    } else {
        // Worth saying out loud rather than swallowing: the image stays pending,
        // so the next reset reverts it. That is the safe direction, but an
        // operator who was told the update worked deserves to know it did not.
        Serial.print("OTA:    WARNING — could not confirm image (err ");
        Serial.print((int)err);
        Serial.println("); the next reset will roll back");
    }
}

static void roll_back(const char* why) {
    s_settled = true;
    Serial.print("OTA:    self-test FAILED (");
    Serial.print(why);
    Serial.println(") — rolling back to the previous firmware");
    Serial.flush();

    // Strapping pins and the relay first. Per src/periph.h external resistors
    // only cover a cold power-on; a warm restart is firmware's job, and this
    // path restarts.
    periph_reset_safe();
    delay(50);

    // Marks the running slot invalid and reboots into the previous one. Does
    // not return.
    esp_ota_mark_app_invalid_rollback_and_reboot();

    // Reached only if there is no previous image to go back to (a board whose
    // other slot is blank — e.g. the first OTA after a serial flash fails its
    // self-test). Nothing to roll back to, so say so and restart rather than
    // falling through into loop() in an undefined state.
    Serial.println("OTA:    no previous image to roll back to — restarting");
    Serial.flush();
    esp_restart();
}

void ota_guard_init() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        // The ordinary case: a serially flashed board, or an image confirmed on
        // an earlier boot. Nothing to prove.
        return;
    }

    s_pending   = true;
    s_start_ms  = millis();
    s_healthy_since = 0;

    Serial.println();
    Serial.print("OTA:    FIRST BOOT of a new image (");
    Serial.print(FW_VERSION_FULL);
    Serial.println(") — on trial");
    Serial.print("        confirming after ");
    Serial.print(OTA_GUARD_SELFTEST_MS / 1000);
    Serial.println("s of BLE reachable + app alive; any reset before then rolls back");
}

void ota_guard_service() {
    if (!s_pending || s_settled) return;

    const uint32_t now = millis();

    if (self_test_ok()) {
        // Require the criteria to hold CONTINUOUSLY, not just to have been true
        // once. A board that advertises, wedges, and recovers has not proven it
        // is stable — and the one thing this gate protects is the ability to
        // reach the board later.
        if (s_healthy_since == 0) s_healthy_since = now;
        if ((uint32_t)(now - s_healthy_since) >= OTA_GUARD_SELFTEST_MS) confirm();
    } else {
        s_healthy_since = 0;
    }

    if (!s_settled && (uint32_t)(now - s_start_ms) >= OTA_GUARD_TIMEOUT_MS) {
#ifdef ENABLE_BLE
        roll_back(ble_config_is_reachable() ? "app task not checking in"
                                            : "BLE never became reachable");
#else
        roll_back("app task not checking in");
#endif
    }
}

bool ota_guard_pending() {
    return s_pending && !s_settled;
}

void ota_guard_print_status(Stream& out) {
    const esp_partition_t* running  = esp_ota_get_running_partition();
    const esp_partition_t* fallback = esp_ota_get_next_update_partition(nullptr);

    out.print("running_slot = ");
    out.println(running ? running->label : "?");
    out.print("next_ota_slot = ");
    out.println(fallback ? fallback->label : "?");

    esp_ota_img_states_t state;
    const char* s = "unknown";
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        switch (state) {
            case ESP_OTA_IMG_NEW:            s = "new";            break;
            case ESP_OTA_IMG_PENDING_VERIFY: s = "pending_verify"; break;
            case ESP_OTA_IMG_VALID:          s = "valid";          break;
            case ESP_OTA_IMG_INVALID:        s = "invalid";        break;
            case ESP_OTA_IMG_ABORTED:        s = "aborted";        break;
            case ESP_OTA_IMG_UNDEFINED:      s = "undefined";      break;
        }
    }
    out.print("image_state = ");
    out.println(s);

    out.print("on_trial = ");
    out.println(ota_guard_pending() ? "yes" : "no");
    if (ota_guard_pending()) {
        out.print("  confirms_in_ms = ");
        const uint32_t held = (s_healthy_since == 0)
                                  ? 0
                                  : (uint32_t)(millis() - s_healthy_since);
        out.println(held >= OTA_GUARD_SELFTEST_MS ? 0
                                                  : OTA_GUARD_SELFTEST_MS - held);
        out.print("  self_test_ok = ");
        out.println(self_test_ok() ? "yes" : "no");
    }
    out.println("OK");
}
