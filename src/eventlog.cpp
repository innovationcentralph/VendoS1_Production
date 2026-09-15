#include "eventlog.h"
#include "counters.h"
#include "rtc.h"
#include "diag.h"
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>

// The partition from partitions_vendo.csv. Subtype 0x40 is the first
// user-defined data subtype — deliberately not `nvs` or `spiffs`, so no
// framework code ever tries to mount, format or garbage-collect it.
#define EVENTLOG_PART_SUBTYPE  ((esp_partition_subtype_t)0x40)
#define EVENTLOG_PART_LABEL    "vendolog"

#define SECTOR_SIZE       4096u
#define SLOTS_PER_SECTOR  (SECTOR_SIZE / EVENTLOG_RECORD_LEN)   // 256

// Depth of the hand-off queue between the app task and loop(). A session ends
// at most every few seconds and the flush runs every 10 ms, so one entry would
// nearly do; 32 is sized for the pathological case of a loop() blocked behind
// something slow, not for normal operation.
#define EVENTLOG_QUEUE_DEPTH  32

// Cap on how long a BLE read or a flush will wait for the state lock. Long
// enough to sit through a 4 KB sector erase (tens of ms) with margin; short
// enough that a wedged holder cannot stall the NimBLE host task indefinitely.
#define EVENTLOG_LOCK_MS  500

// An erased flash word. A slot whose seq reads as this has never been written.
#define SEQ_ERASED  0xFFFFFFFFu

// One closed session, on its way from the app task to the flash write in loop().
struct PendingSession {
    uint32_t ts;
    uint32_t cents;
    uint8_t  ts_unverified;
};

// The session currently being billed. App task only, deliberately unlocked —
// see eventlog_note_billed() in the header for why a mutex here would be worse
// than none.
static uint32_t s_open_cents = 0;
static uint32_t s_open_ts    = 0;
static uint8_t  s_open_unv   = 0;

static const esp_partition_t* s_part       = nullptr;
static QueueHandle_t          s_queue      = nullptr;
static SemaphoreHandle_t      s_lock       = nullptr;

static uint32_t s_capacity   = 0;   // slots in the ring
static uint32_t s_head       = 0;   // slot the NEXT record goes into
static uint32_t s_next_seq   = 1;   // seq the NEXT record gets
static uint32_t s_oldest_seq = 0;   // 0 while the log is empty
static uint32_t s_count      = 0;   // records currently on flash
static uint32_t s_dropped    = 0;   // sessions lost to a full queue
static uint32_t s_write_fail = 0;   // flash writes that failed
static uint32_t s_wraps      = 0;   // sectors erased to make room
static bool     s_fractional = false; // a session billed a fractional peso amount

struct Lock {
    bool held;
    Lock() : held(false) {
        if (s_lock) held = (xSemaphoreTake(s_lock, pdMS_TO_TICKS(EVENTLOG_LOCK_MS)) == pdTRUE);
    }
    ~Lock() { if (held) xSemaphoreGive(s_lock); }
};

// ============================================================================
// Record encode/decode
// ============================================================================

static void put_u32(uint8_t* b, size_t off, uint32_t v) {
    b[off]     = (uint8_t)(v & 0xFF);
    b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t get_u32(const uint8_t* b, size_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

// Fletcher-16. Cheap, and unlike a plain sum it catches transposed bytes —
// which matters here because the fields it covers are mostly little-endian
// integers that differ from their neighbours by a byte order.
static uint16_t fletcher16(const uint8_t* d, size_t n) {
    uint16_t a = 0, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (uint16_t)((a + d[i]) % 255);
        b = (uint16_t)((b + a) % 255);
    }
    return (uint16_t)((b << 8) | a);
}

// True if the 16 bytes are a complete, uncorrupted record. Rejects both a
// never-written slot (all 0xFF) and a write torn by a reset partway through.
static bool record_valid(const uint8_t rec[EVENTLOG_RECORD_LEN], uint32_t* seq_out) {
    const uint32_t seq = get_u32(rec, 0);
    if (seq == SEQ_ERASED) return false;
    const uint16_t want = (uint16_t)(rec[14] | (rec[15] << 8));
    if (want != fletcher16(rec, EVENTLOG_WIRE_LEN)) return false;
    if (seq_out) *seq_out = seq;
    return true;
}

static bool slot_read(uint32_t slot, uint8_t rec[EVENTLOG_RECORD_LEN]) {
    return esp_partition_read(s_part, (size_t)slot * EVENTLOG_RECORD_LEN,
                              rec, EVENTLOG_RECORD_LEN) == ESP_OK;
}

// ============================================================================
// Boot scan
//
// Reads the whole 128 KB partition once. That is ~6 ms of cached flash reads,
// paid once at boot, and it buys the three anchors everything else is derived
// from arithmetically: the newest seq (and its slot), the oldest surviving seq,
// and how many records are live. A cleverer incremental scheme would save
// milliseconds nobody is counting and lose the property that a corrupted ring
// is detected at boot rather than at the first sync.
// ============================================================================

// Set by scan_locked() when the partition contains bytes that are not 0xFF —
// whether or not any of them parse as a record. See eventlog_init().
static bool s_scan_saw_data = false;

static void scan_locked() {
    uint8_t chunk[EVENTLOG_RECORD_LEN * 16];   // 256 B — 16 records per read

    uint32_t max_seq = 0, max_slot = 0;
    uint32_t min_seq = 0;
    uint32_t count = 0;
    bool     any = false;
    // Highest slot that is not erased, VALID OR NOT. The head must clear a torn
    // record too: flash bits only go 1->0, so writing into a slot that already
    // holds garbage produces more garbage rather than a good record.
    int64_t  last_used = -1;

    for (uint32_t base = 0; base < s_capacity; base += 16) {
        if (esp_partition_read(s_part, (size_t)base * EVENTLOG_RECORD_LEN,
                               chunk, sizeof(chunk)) != ESP_OK) {
            continue;
        }
        for (uint32_t i = 0; i < 16; ++i) {
            const uint8_t* rec = chunk + i * EVENTLOG_RECORD_LEN;
            const uint32_t slot = base + i;
            if (get_u32(rec, 0) != SEQ_ERASED) last_used = (int64_t)slot;

            uint32_t seq;
            if (!record_valid(rec, &seq)) continue;
            ++count;
            if (!any || seq > max_seq) { max_seq = seq; max_slot = slot; }
            if (!any || seq < min_seq) { min_seq = seq; }
            any = true;
        }
    }

    s_count         = count;
    s_oldest_seq    = any ? min_seq : 0;
    s_next_seq      = any ? max_seq + 1 : 1;
    s_scan_saw_data = (last_used >= 0);

    int64_t head_from = (int64_t)max_slot;
    if (!any) head_from = last_used;             // torn records but no good one
    else if (last_used > head_from) head_from = last_used;
    s_head = (uint32_t)((head_from + 1) % (int64_t)s_capacity);
}

// ============================================================================
// Write path — loop() context only
// ============================================================================

// Erases the sector `slot` falls at the start of, discarding its 256 records.
// This is the rolling window: the oldest sector makes room for the newest.
// Reads the sector first so the survivors' boundary (`oldest_seq`) is exact
// rather than assumed — costs one 4 KB read per 256 events.
static void erase_sector_for_locked(uint32_t slot) {
    const uint32_t sector = slot / SLOTS_PER_SECTOR;
    uint8_t chunk[EVENTLOG_RECORD_LEN * 16];

    uint32_t victim_max = 0, victims = 0;
    for (uint32_t i = 0; i < SLOTS_PER_SECTOR; i += 16) {
        const size_t off = (size_t)(sector * SECTOR_SIZE) + (size_t)i * EVENTLOG_RECORD_LEN;
        if (esp_partition_read(s_part, off, chunk, sizeof(chunk)) != ESP_OK) continue;
        for (uint32_t k = 0; k < 16; ++k) {
            uint32_t seq;
            if (!record_valid(chunk + k * EVENTLOG_RECORD_LEN, &seq)) continue;
            ++victims;
            if (seq > victim_max) victim_max = seq;
        }
    }

    if (esp_partition_erase_range(s_part, (size_t)sector * SECTOR_SIZE, SECTOR_SIZE) != ESP_OK) {
        diag_raise(DIAG_ERR_NVS_WRITE);
        return;
    }

    if (victims > 0) {
        // Everything that survived is newer than the newest record just
        // destroyed. A client whose cursor is below this lost rows — see the
        // header: the backend detects that as firstSeq > lastSeq + 1.
        s_oldest_seq = victim_max + 1;
        s_count      = (s_count > victims) ? (s_count - victims) : 0;
        ++s_wraps;
    }
}

static bool write_one_locked(const PendingSession& p) {
    // Centavos -> pesos, the one place it happens for this characteristic.
    const uint32_t pesos = p.cents / 100u;
    if ((p.cents % 100u) != 0) s_fractional = true;

    if (s_head % SLOTS_PER_SECTOR == 0) erase_sector_for_locked(s_head);

    uint8_t rec[EVENTLOG_RECORD_LEN];
    put_u32(rec, 0, s_next_seq);
    put_u32(rec, 4, p.ts);
    rec[8] = p.ts_unverified;
    // denom is the COIN's face value in a frame designed around coins. A session
    // has none, so 0 — a value that cannot be mistaken for a real denomination,
    // and the filter the app needs to keep these rows out of its coin breakdown.
    // See the header, and D18 in docs/APP_BLE_PLAN.md.
    rec[9] = 0;
    put_u32(rec, 10, pesos);
    const uint16_t fc = fletcher16(rec, EVENTLOG_WIRE_LEN);
    rec[14] = (uint8_t)(fc & 0xFF);
    rec[15] = (uint8_t)(fc >> 8);

    if (esp_partition_write(s_part, (size_t)s_head * EVENTLOG_RECORD_LEN,
                            rec, EVENTLOG_RECORD_LEN) != ESP_OK) {
        // seq is NOT consumed and the head does NOT advance — see the header.
        // A failed write must leave no hole for the backend to misread as a
        // buffer overrun.
        ++s_write_fail;
        diag_raise(DIAG_ERR_NVS_WRITE);
        return false;
    }

    if (s_oldest_seq == 0) s_oldest_seq = s_next_seq;
    if (s_count < s_capacity) ++s_count;

    // Live Counters reports last_seq, and the app falls back to it for the
    // sync_ack when a pull returned no events. Keeping it current here (rather
    // than only at boot) is what makes that fallback correct.
    counters_set_last_seq(s_next_seq);

    ++s_next_seq;
    s_head = (s_head + 1) % s_capacity;
    return true;
}

// ============================================================================
// Public API
// ============================================================================

void eventlog_init() {
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(EVENTLOG_QUEUE_DEPTH, sizeof(PendingSession));
    if (s_lock == nullptr || s_queue == nullptr) {
        Serial.println("WARN: event log init failed (no RAM) — Session Log will be empty");
        s_part = nullptr;
        return;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, EVENTLOG_PART_SUBTYPE,
                                      EVENTLOG_PART_LABEL);
    if (s_part == nullptr) {
        // The board was flashed with a partition table that has no vendolog —
        // the stock default.csv, i.e. a build before this feature. Say so
        // loudly: the symptom otherwise is an app that syncs zero events
        // forever with nothing to explain it.
        Serial.println("ERROR: `vendolog` partition not found — Session Log (f004) DISABLED");
        Serial.println("       reflash with board_build.partitions = partitions_vendo.csv");
        return;
    }

    s_capacity = (uint32_t)(s_part->size / EVENTLOG_RECORD_LEN);

    {
        Lock lock;
        scan_locked();

        // A partition that holds bytes but no readable record has never been
        // ours: the region was SPIFFS in the stock table, and a repartition
        // does not erase what was there. Flash writes can only clear bits, so
        // writing records into dirty slots produces garbage that then fails its
        // own checksum — a log that silently records nothing until the ring has
        // turned over once. Erase it now instead. ~0.8 s, once, on the first
        // boot after the repartition.
        if (s_count == 0 && s_scan_saw_data) {
            Serial.println("Log:    `vendolog` holds foreign data (was SPIFFS?) — erasing once");
            if (esp_partition_erase_range(s_part, 0, s_part->size) == ESP_OK) {
                scan_locked();
            } else {
                diag_raise(DIAG_ERR_NVS_WRITE);
                Serial.println("ERROR: erase failed — Session Log will not record");
            }
        }

        // The floor that stops an erased or freshly-partitioned log from
        // rewinding seq into ids the backend has already filed. counters keeps
        // it in its own NVS namespace for exactly this reason.
        const uint32_t floor_seq = counters_last_seq();
        if (s_next_seq <= floor_seq) s_next_seq = floor_seq + 1;
    }

    Serial.print("Log:    ");
    Serial.print(s_count);
    Serial.print(" events, capacity ");
    Serial.print(s_capacity);
    Serial.print(", next seq ");
    Serial.println(s_next_seq);
}

bool eventlog_ready() { return s_part != nullptr; }

void eventlog_note_billed(uint32_t cents) {
    if (cents == 0) return;

    if (s_open_cents == 0) {
        // First money billed into this session, so this is the session's time.
        //
        // rtc_now() only, which is time() plus a "has the clock ever been set"
        // flag. NOT rtc_valid(): that one bit-bangs the SLM1302 (~350 us) and
        // takes the RTC mutex, and src/rtc.h is explicit that a hardware read
        // has no business in the vend path. rtc_now() returning 0 IS the "never
        // established" signal — the same test counters.cpp uses for its
        // unknown-business-day key — and that is what ts_unverified tells the
        // backend, so it excludes the row from DailyStat rather than filing it
        // under 1970.
        s_open_ts  = rtc_now();
        s_open_unv = (s_open_ts == 0) ? 1 : 0;
    }

    // Saturating, for the same reason counters.cpp saturates: a total stuck at
    // the ceiling is a visible anomaly, one that wraps reads as plausible.
    if (s_open_cents > UINT32_MAX - cents) s_open_cents = UINT32_MAX;
    else                                   s_open_cents += cents;
}

void eventlog_record_session() {
    if (s_queue == nullptr) return;
    if (s_open_cents == 0) return;      // nothing was billed — not a session

    PendingSession p;
    p.ts            = s_open_ts;
    p.cents         = s_open_cents;
    p.ts_unverified = s_open_unv;

    // Cleared whether or not the queue accepts it. A session that cannot be
    // queued is lost, and carrying its money into the NEXT session would report
    // one customer's takings under another customer's timestamp — worse than
    // the gap, and invisible.
    s_open_cents = 0;
    s_open_ts    = 0;
    s_open_unv   = 0;

    // Zero timeout — the app task must never block here. A full queue means
    // loop() has been stalled for seconds, which is its own fault; dropping the
    // row without consuming a seq is the honest failure.
    if (xQueueSend(s_queue, &p, 0) != pdTRUE) {
        ++s_dropped;
        diag_raise(DIAG_ERR_NVS_WRITE);
    }
}

void eventlog_service() {
    if (s_part == nullptr || s_queue == nullptr) return;

    PendingSession p;
    while (xQueuePeek(s_queue, &p, 0) == pdTRUE) {
        Lock lock;
        if (!lock.held) return;                    // try again next tick
        if (!write_one_locked(p)) return;          // keep it queued, retry later
        xQueueReceive(s_queue, &p, 0);             // consume only once written
    }
}

size_t eventlog_build_page(uint32_t after_seq, uint8_t* out) {
    out[0] = 0;   // count
    out[1] = 0;   // has_more

    if (s_part == nullptr) return 2;
    Lock lock;
    if (!lock.held) return 2;
    if (s_count == 0) return 2;

    const uint32_t last = s_next_seq - 1;
    if (after_seq >= last) return 2;               // caller is already current

    // after_seq is EXCLUSIVE on every page — the app advances its cursor to the
    // last seq it received, not that plus one. Treating it as inclusive drops
    // exactly one event per page.
    uint32_t seq = after_seq + 1;
    if (seq < s_oldest_seq) seq = s_oldest_seq;    // the rest was wrapped away

    uint8_t n = 0;
    for (; seq <= last && n < EVENTLOG_PAGE_MAX_EVENTS; ++seq, ++n) {
        const uint32_t slot = (uint32_t)((uint64_t)s_head + s_capacity - (s_next_seq - seq)) % s_capacity;
        uint8_t rec[EVENTLOG_RECORD_LEN];
        uint32_t got;
        if (!slot_read(slot, rec) || !record_valid(rec, &got) || got != seq) {
            // The seq->slot invariant did not hold. Serve what is already in
            // the page rather than bytes that belong to a different event: a
            // short page is a slow sync, a wrong page is wrong money.
            break;
        }
        memcpy(out + 2 + (size_t)n * EVENTLOG_WIRE_LEN, rec, EVENTLOG_WIRE_LEN);
    }

    out[0] = n;
    out[1] = (n > 0 && seq <= last) ? 1 : 0;
    return 2 + (size_t)n * EVENTLOG_WIRE_LEN;
}

uint32_t eventlog_last_seq()   { Lock l; return s_next_seq > 0 ? s_next_seq - 1 : 0; }
uint32_t eventlog_oldest_seq() { Lock l; return s_oldest_seq; }
uint32_t eventlog_count()      { Lock l; return s_count; }

// ============================================================================
// CLI reporting
// ============================================================================

void eventlog_print(Stream& out) {
    if (s_part == nullptr) {
        out.println("event log DISABLED — `vendolog` partition not found");
        out.println("  reflash with board_build.partitions = partitions_vendo.csv");
        return;
    }

    Lock lock;
    out.print("partition = "); out.print(EVENTLOG_PART_LABEL);
    out.print(" @0x"); out.print((unsigned)s_part->address, HEX);
    out.print(" size "); out.print((unsigned)(s_part->size / 1024)); out.println(" KB");

    out.print("capacity  = "); out.print(s_capacity); out.println(" sessions");
    out.print("stored    = "); out.print(s_count);
    out.print("  ("); out.print((s_count * 100) / (s_capacity ? s_capacity : 1));
    out.println("% full)");

    if (s_count == 0) {
        out.println("range     = (empty)");
    } else {
        out.print("range     = seq "); out.print(s_oldest_seq);
        out.print(" .. "); out.println(s_next_seq - 1);
    }
    out.print("next seq  = "); out.println(s_next_seq);

    out.print("wraps     = "); out.print(s_wraps);
    out.println("  (sectors erased to make room; each one dropped up to 256 sessions)");
    if (s_wraps > 0) {
        out.println("  a client whose cursor is below the oldest seq above has lost rows;");
        out.println("  the backend sees that as firstSeq > lastSeq + 1");
    }

    if (s_dropped > 0) {
        out.print("DROPPED   = "); out.print(s_dropped);
        out.println("  sessions lost to a full queue — loop() was stalled");
    }
    if (s_write_fail > 0) {
        out.print("WRITE FAIL= "); out.print(s_write_fail);
        out.println("  flash writes rejected (diagnostics 0x04)");
    }
    if (s_open_cents > 0) {
        out.print("open      = P"); out.print(s_open_cents / 100u);
        out.println("  billed into a session that has not ended yet (not on flash)");
    }
    if (s_fractional) {
        out.println("WARNING: a session billed a fractional number of pesos —");
        out.println("         the Session Log wire has no centavos field and rounds DOWN");
    }
}

void eventlog_dump(Stream& out, uint32_t after_seq, uint32_t max_rows) {
    if (s_part == nullptr) { out.println("event log DISABLED"); return; }

    Lock lock;
    if (s_count == 0) { out.println("(empty)"); return; }

    const uint32_t last = s_next_seq - 1;
    uint32_t seq = after_seq + 1;
    if (seq < s_oldest_seq) seq = s_oldest_seq;

    out.println("   seq | ts (epoch UTC) | unv | denom | amount   (one row = one session)");
    uint32_t shown = 0;
    for (; seq <= last && shown < max_rows; ++seq, ++shown) {
        const uint32_t slot = (uint32_t)((uint64_t)s_head + s_capacity - (s_next_seq - seq)) % s_capacity;
        uint8_t rec[EVENTLOG_RECORD_LEN];
        uint32_t got;
        if (!slot_read(slot, rec) || !record_valid(rec, &got) || got != seq) {
            out.print("  seq "); out.print(seq); out.println(": UNREADABLE — ring invariant broken");
            break;
        }
        char line[64];
        snprintf(line, sizeof(line), "%6lu | %14lu |  %u  | %5u | P%lu",
                 (unsigned long)got, (unsigned long)get_u32(rec, 4), rec[8], rec[9],
                 (unsigned long)get_u32(rec, 10));
        out.println(line);
    }
    if (seq <= last) {
        out.print("... "); out.print(last - seq + 1); out.println(" more");
    }
}
