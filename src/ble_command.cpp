#include "ble_command.h"
#include "app.h"
#include "config.h"
#include "diag.h"
#include "periph.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define BLE_COMMAND_CHAR_UUID "6a40f007-0000-1000-8000-00805f9b0001"

// Wire frame: op:u8 + param:u32 LE. Exactly 5 bytes — encodeCommand() always
// sends 5, and a short write is a malformed one rather than an op with a
// default param.
#define CMD_WIRE_LEN 5

// Depth of the mailbox. The app sends at most two commands per connect
// (sync_ack, then clear_errors), and the Diagnostics wizard fires one at a time
// while a technician watches. 8 is slack for a stuck loop(), not a design need.
#define CMD_QUEUE_DEPTH 8

struct PendingCommand {
    uint8_t  op;
    uint32_t param;
};

static NimBLECharacteristic* s_char  = nullptr;
static QueueHandle_t         s_queue = nullptr;

static uint32_t s_last_acked_seq = 0;
static bool     s_ever_acked     = false;

static uint32_t get_u32(const uint8_t* b, size_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static const char* op_name(uint8_t op) {
    switch (op) {
        case CMD_OP_SYNC_ACK:     return "sync_ack";
        case CMD_OP_CLEAR_ERRORS: return "clear_errors";
        case CMD_OP_IDENTIFY:     return "identify";
        case CMD_OP_TEST_RELAY:   return "test_relay";
        case CMD_OP_TEST_BUZZER:  return "test_buzzer";
        case CMD_OP_TEST_LED:     return "test_led";
        case CMD_OP_WIFI_FORGET:  return "wifi_forget";
        case CMD_OP_TEST_USER_LED: return "test_user_led";
        case CMD_OP_TEST_MODE:     return "test_mode";
        case CMD_OP_TEST_COIN_SLOT: return "test_coin_slot";
        case CMD_OP_TEST_BUTTON:   return "test_button";
        default:                  return "unknown";
    }
}

// True for the ops that drive a physical output and must not fire during a
// paid session. sync_ack, clear_errors and wifi_forget are bookkeeping and run
// at any time — refusing a sync_ack mid-vend would fail the app's connect for
// no reason at all.
static bool op_is_physical(uint8_t op) {
    return op == CMD_OP_IDENTIFY || op == CMD_OP_TEST_RELAY ||
           op == CMD_OP_TEST_BUZZER || op == CMD_OP_TEST_LED ||
           op == CMD_OP_TEST_USER_LED;
}

// test_mode and test_coin_slot are gated, but not by op_is_physical: their rule
// depends on the param. Entering test mode needs an idle board; leaving must
// work from anywhere, including from a state the generic gate would refuse -
// otherwise the exit op could be locked out by the very mode it exits.
static bool op_has_own_gate(uint8_t op) {
    return op == CMD_OP_TEST_MODE || op == CMD_OP_TEST_COIN_SLOT ||
           op == CMD_OP_TEST_BUTTON;
}

// ============================================================================
// Execution — loop() context
// ============================================================================

// The beep helpers take an AppConfig for tone and duration. The stored config
// is the right source: a technician pressing "test buzzer" wants to hear what
// this machine actually sounds like, not a firmware-chosen tone.
static void stored_cfg(AppConfig* cfg) {
    if (!config_load_quiet(cfg)) config_defaults(cfg);
}

static void execute(const PendingCommand& c) {
    // Any command at all keeps test mode alive (see TEST_MODE_IDLE_TIMEOUT_MS).
    // Before the gate, so even a refused op counts as the technician still being
    // there - dropping out of test mode because the one op they tried was
    // refused would be its own surprise.
    app_test_mode_poke();

    if (op_is_physical(c.op) && !op_has_own_gate(c.op) && !app_state_allows_physical_op()) {
        // Dropped, not deferred — running it minutes later, after the operator
        // has stopped watching, is worse than not running it. Invisible to the
        // app by construction (write-only characteristic); serial is the only
        // channel there is.
        Serial.print("BLE command ");
        Serial.print(op_name(c.op));
        Serial.println(" REFUSED — a session is in progress");
        return;
    }

    AppConfig cfg;

    switch (c.op) {
        case CMD_OP_SYNC_ACK:
            // Recorded, never applied. See the header: writing this into the
            // board's own last_seq would let a stale server value rewind the
            // sequence numbers delta sync depends on.
            s_last_acked_seq = c.param;
            s_ever_acked     = true;
            Serial.print("BLE sync_ack — backend has everything up to seq ");
            Serial.println(c.param);
            break;

        case CMD_OP_CLEAR_ERRORS:
            // What the app sends once it has uploaded the fault list. Without
            // it f006's faults re-upload on every connect forever. Note 0x03
            // (RTC unset) re-raises immediately if the clock is still unset —
            // it is a present condition, not a historical event.
            diag_clear_all();
            Serial.println("BLE clear_errors — fault list emptied");
            break;

        case CMD_OP_IDENTIFY:
            // "Which machine am I connected to?" — must be visible from across
            // a room and audible over an arcade, because that is the entire
            // job. It does NOT use buzzer_beep_enter_config(): that is an empty
            // stub on this board and on the STM32 (see periph.cpp, and CLAUDE.md
            // on inherited stubs), which made identify silent AND its LED pulse
            // a few microseconds wide — leds_set(true), an instant-return stub,
            // leds_set(false). Implementing the config beep to fix this would
            // diverge the two firmwares over a cosmetic, so the pattern lives
            // here instead.
            //
            // buzzer_beep_denied() is the only real tone that is not a vend
            // tone (one 400 Hz buzz, vs. start 2x1000 Hz and end 3x2500 Hz), so
            // nobody standing at the machine mistakes identify for a session
            // starting. The blink is 5x100 ms, distinguishable by eye from
            // test_led's slower 3x200 ms.
            buzzer_beep_denied();
            for (int i = 0; i < 5; ++i) {
                leds_set(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                leds_set(false);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            Serial.println("BLE identify");
            break;

        case CMD_OP_TEST_RELAY:
            Serial.println("BLE test_relay — pulsing relay");
            relay_on();
            vTaskDelay(pdMS_TO_TICKS(CMD_RELAY_TEST_MS));
            relay_off();
            break;

        case CMD_OP_TEST_BUZZER:
            stored_cfg(&cfg);
            buzzer_beep_session_start(&cfg);
            Serial.println("BLE test_buzzer");
            break;

        case CMD_OP_TEST_LED:
            for (int i = 0; i < 3; ++i) {
                leds_set(true);
                vTaskDelay(pdMS_TO_TICKS(200));
                leds_set(false);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            Serial.println("BLE test_led");
            break;

        case CMD_OP_TEST_USER_LED:
            // Handed to the app task rather than run here: it owns PIN_USER_LED
            // and re-asserts it every 20 ms in IDLE, so a blink from loop()
            // would be overwritten before anyone saw it. See
            // app_request_user_led_test() in app.h. The idle gate above has
            // already passed, and the app task honours the request in IDLE
            // only, so the two agree even if the state changes in between.
            app_request_user_led_test();
            Serial.println("BLE test_user_led — handed to the app task");
            break;

        case CMD_OP_TEST_MODE:
            if (c.param != 0) {
                // Entering needs an idle board. Refusing here rather than in
                // the app task is what keeps the request flag from ever being
                // set mid-session (see APP_STATE_IDLE in app.cpp).
                if (!app_state_is_idle()) {
                    Serial.println("BLE test_mode ENTER REFUSED - board is not idle");
                    break;
                }
                app_request_test_mode(true);
                Serial.println("BLE test_mode ENTER - vend operation will suspend");
            } else {
                // Always accepted, from any state. An exit that could be
                // refused is not an exit.
                app_request_test_mode(false);
                Serial.println("BLE test_mode LEAVE");
            }
            break;

        case CMD_OP_TEST_COIN_SLOT:
            // engaged(), not active(): see app.h - the app sends this
            // immediately after the enter op, before the app task has
            // published the new state.
            if (!app_test_mode_engaged()) {
                // Outside test mode the coin slot belongs to the vend state
                // machine, which would fight us for IO12 - and losing that
                // fight in the wrong direction means taking money no session
                // will bill. See ble_command.h.
                Serial.println("BLE test_coin_slot REFUSED - only valid in test mode");
                break;
            }
            app_request_test_coin_slot(c.param != 0);
            Serial.print("BLE test_coin_slot - ");
            Serial.println(c.param != 0 ? "ENABLE (count restarts at 0)" : "INHIBIT");
            break;

        case CMD_OP_TEST_BUTTON:
            // Resets a counter; drives nothing. Refused outside test mode only
            // because the counter has no meaning there - START is the session
            // button everywhere else, and a count of "sessions a customer
            // started" is not what anyone asking for this wants.
            if (!app_test_mode_engaged()) {
                Serial.println("BLE test_button REFUSED - only valid in test mode");
                break;
            }
            app_request_test_button_reset();
            Serial.println("BLE test_button - press count reset to 0");
            break;

        case CMD_OP_WIFI_FORGET:
            // No WiFi on this board yet (f00b/f00c unbuilt). Accepted and
            // ignored rather than refused: the app sends it as an explicit
            // user action, and "nothing to forget" is the honest outcome, not
            // an error. Becomes real work when WiFi lands.
            Serial.println("BLE wifi_forget — no WiFi on this board, nothing to clear");
            break;

        default:
            Serial.print("BLE command: unknown op 0x");
            Serial.println(c.op, HEX);
            break;
    }
}

// ============================================================================
// GATT surface
// ============================================================================

class CommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.length() != CMD_WIRE_LEN) {
            Serial.print("BLE command: ignored, expected 5 bytes (op:u8 + param:u32), got ");
            Serial.println((unsigned)v.length());
            return;
        }

        const uint8_t* b = (const uint8_t*)v.data();
        PendingCommand cmd;
        cmd.op    = b[0];
        cmd.param = get_u32(b, 1);

        if (s_queue == nullptr) return;
        // Zero timeout — the NimBLE host task must never block here, and the
        // client is waiting on the ATT response to this write.
        if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
            Serial.print("BLE command: queue full, dropped ");
            Serial.println(op_name(cmd.op));
        }
    }
};

void ble_command_register(NimBLEService* service) {
    s_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(PendingCommand));
    if (s_queue == nullptr) {
        Serial.println("WARN: BLE command queue creation failed — f007 will accept nothing");
        return;
    }
    // WRITE, not WRITE_NR: BleService.sendCommand() goes through write(), which
    // is writeCharacteristicWithResponseForService — an ATT Write Request. A
    // characteristic offering only write-without-response would reject it.
    s_char = service->createCharacteristic(BLE_COMMAND_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
    s_char->setCallbacks(new CommandCallbacks());
}

void ble_command_service() {
    if (s_queue == nullptr) return;
    PendingCommand cmd;
    // One per call, not a drain loop: each physical op blocks for a few hundred
    // milliseconds, and running a queue of them back to back would hold loop()
    // for seconds — delaying the event-log flush and the CLI for no good reason.
    if (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) execute(cmd);
}

uint32_t ble_command_last_acked_seq() { return s_last_acked_seq; }
bool     ble_command_ever_acked()     { return s_ever_acked; }
