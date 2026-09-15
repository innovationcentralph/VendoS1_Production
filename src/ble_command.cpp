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
        default:                  return "unknown";
    }
}

// True for the ops that drive a physical output and must not fire during a
// paid session. sync_ack, clear_errors and wifi_forget are bookkeeping and run
// at any time — refusing a sync_ack mid-vend would fail the app's connect for
// no reason at all.
static bool op_is_physical(uint8_t op) {
    return op == CMD_OP_IDENTIFY || op == CMD_OP_TEST_RELAY ||
           op == CMD_OP_TEST_BUZZER || op == CMD_OP_TEST_LED;
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
    if (op_is_physical(c.op) && !app_state_is_idle()) {
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
            // "Which machine am I connected to?" — deliberately the config
            // beep rather than a session beep, so nobody standing at the
            // machine mistakes it for a vend starting.
            stored_cfg(&cfg);
            leds_set(true);
            buzzer_beep_enter_config();
            leds_set(false);
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
