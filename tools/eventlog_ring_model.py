# Event-log ring model - `python tools/eventlog_ring_model.py`
#
# WHY THIS EXISTS AS PYTHON RATHER THAN A UNIT TEST
#
# There is no host toolchain in this environment (only the xtensa cross
# compiler), so src/eventlog.cpp cannot be compiled and run on a PC, and no
# board has executed this firmware yet. This is a line-for-line transliteration
# of that file's ring arithmetic, so the DESIGN can at least be exercised:
# oldest-first wrap, the seq->slot formula, the exclusive cursor, and the app's
# own pagination loop from BleService.pullSessionLogDelta().
#
# WHAT IT DOES NOT PROVE: that the C++ is a faithful copy of this model, that
# esp_partition writes land where they should, or anything at all about flash.
# If eventlog.cpp's arithmetic changes, change it here too and re-run.
#

# Transliteration of src/eventlog.cpp's ring arithmetic, to exercise the wrap /
# pagination logic that cannot be run on hardware yet. Mirrors the C++ line for
# line: same erase-at-sector-start, same oldest recompute, same slot formula,
# same exclusive-cursor page build.
SECTOR = 4096
REC = 16
SLOTS_PER_SECTOR = SECTOR // REC          # 256
CAP = (128 * 1024) // REC                 # 8192
PAGE = 10

class Ring:
    def __init__(self, seq_floor=0):
        self.flash = [None] * CAP         # None == erased slot
        self.head = 0
        self.next_seq = max(1, seq_floor + 1)
        self.oldest = 0
        self.count = 0
        self.wraps = 0

    def _erase_sector_for(self, slot):
        sector = slot // SLOTS_PER_SECTOR
        base = sector * SLOTS_PER_SECTOR
        victims = [r for r in self.flash[base:base + SLOTS_PER_SECTOR] if r is not None]
        for i in range(base, base + SLOTS_PER_SECTOR):
            self.flash[i] = None
        if victims:
            self.oldest = max(v['seq'] for v in victims) + 1
            self.count = max(0, self.count - len(victims))
            self.wraps += 1

    # One row = one SESSION (one paid period), carrying the total billed for it.
    # `cents` is what the whole session consumed, not one coin pulse.
    def record(self, cents, ts=0, unverified=0):
        if self.head % SLOTS_PER_SECTOR == 0:
            self._erase_sector_for(self.head)
        pesos = cents // 100
        self.flash[self.head] = {'seq': self.next_seq, 'ts': ts,
                                 'unv': unverified,
                                 # denom is 0 on every row: a session has no coin
                                 # face value. See src/eventlog.h.
                                 'denom': 0, 'amount': pesos}
        if self.oldest == 0:
            self.oldest = self.next_seq
        if self.count < CAP:
            self.count += 1
        self.next_seq += 1
        self.head = (self.head + 1) % CAP

    def build_page(self, after_seq):
        if self.count == 0:
            return [], False
        last = self.next_seq - 1
        if after_seq >= last:
            return [], False
        seq = after_seq + 1
        if seq < self.oldest:
            seq = self.oldest
        out = []
        while seq <= last and len(out) < PAGE:
            slot = (self.head + CAP - (self.next_seq - seq)) % CAP
            rec = self.flash[slot]
            assert rec is not None and rec['seq'] == seq, \
                f"slot arithmetic broke: want {seq}, slot {slot} holds {rec}"
            out.append(rec)
            seq += 1
        return out, (len(out) > 0 and seq <= last)

    # The app's loop, verbatim from BleService.pullSessionLogDelta()
    def pull(self, after_seq, max_pages=500):
        events, cursor = [], after_seq
        for _ in range(max_pages):
            page, has_more = self.build_page(cursor)
            events.extend(page)
            if not has_more or not page:
                break
            cursor = page[-1]['seq']       # LAST SEQ RECEIVED, not +1
        return events


def check(name, cond):
    print(("PASS  " if cond else "FAIL  ") + name)
    return cond

ok = True

# 1. Empty log
r = Ring()
ok &= check("empty log yields no events, has_more=0", r.build_page(0) == ([], False))

# 2. Under capacity, single pull delivers everything exactly once, in order
r = Ring()
for i in range(1500):
    r.record(100)
got = r.pull(0)
ok &= check("1500 rows pulled exactly once in order",
            [e['seq'] for e in got] == list(range(1, 1501)))
ok &= check("no wrap under capacity", r.wraps == 0 and r.count == 1500)

# 3. Incremental sync: pull, add, pull again — no repeats, no drops
last = got[-1]['seq']
for i in range(37):
    r.record(500)
got2 = r.pull(last)
ok &= check("delta pull returns only the 37 new events",
            [e['seq'] for e in got2] == list(range(1501, 1538)))

# 4. The exclusive-cursor trap: cursor is the last seq RECEIVED
r = Ring()
for i in range(25):
    r.record(100)
p1, more1 = r.build_page(0)
p2, more2 = r.build_page(p1[-1]['seq'])
ok &= check("page 1 = seq 1..10, page 2 = seq 11..20 (no event dropped at the seam)",
            [e['seq'] for e in p1] == list(range(1, 11)) and
            [e['seq'] for e in p2] == list(range(11, 21)) and more1 and more2)

# 5. Wrap: oldest-first, and what survives is contiguous and newest
r = Ring()
N = 25000
for i in range(N):
    r.record(100)
ok &= check("wrap happened", r.wraps > 0)
ok &= check("retained window is contiguous up to the newest",
            r.oldest + r.count - 1 == r.next_seq - 1)
ok &= check(f"retained {r.count} events (>= 31 sectors' worth)",
            r.count >= 31 * SLOTS_PER_SECTOR)
# Unbounded pull (the app caps at 500 pages — see check 7 for that interaction)
survivors = r.pull(0, max_pages=10**6)
ok &= check("a from-scratch pull returns exactly the retained window",
            [e['seq'] for e in survivors] == list(range(r.oldest, r.next_seq)))

# 6. A client offline longer than the ring: gap is detectable, nothing is fabricated
stale_cursor = r.oldest - 500          # backend's lastSeq, long behind
delta = r.pull(stale_cursor)
first_seq = delta[0]['seq']
ok &= check("stale client gets the oldest retained event, not garbage",
            first_seq == r.oldest)
ok &= check("gap is visible server-side as firstSeq > lastSeq + 1",
            first_seq > stale_cursor + 1)

# 7. The app's 500-page cap vs a full ring
r = Ring()
for i in range(CAP):
    r.record(100)
one_sync = r.pull(0, max_pages=500)
ok &= check(f"one connect drains {len(one_sync)} events; a full ring ({r.count}) needs more than one",
            len(one_sync) == 5000 and r.count > 5000)
resumed = r.pull(one_sync[-1]['seq'], max_pages=500)
ok &= check("the next connect resumes exactly where the first stopped",
            resumed[0]['seq'] == one_sync[-1]['seq'] + 1)

# 8. seq floor from counters (erased log must not rewind)
r = Ring(seq_floor=9_000_000)
r.record(100)
ok &= check("fresh ring seeded above counters_last_seq()",
            r.build_page(0)[0][0]['seq'] == 9_000_001)

# 9. Money: centavos in, pesos out; denom is always 0; amount does not saturate
r = Ring()
r.record(1500)         # a P15 session (P5 + P10 coins, 15 pulses at P1)
r.record(50_000)       # a P500 session — well beyond a u8
evs = r.pull(0)
ok &= check("P15 session -> one row, amount 15, denom 0",
            len(evs) == 2 and evs[0]['amount'] == 15 and evs[0]['denom'] == 0)
ok &= check("P500 session -> amount 500 (u32, no saturation), denom still 0",
            evs[1]['amount'] == 500 and evs[1]['denom'] == 0)

# 10. The scenario that drove the decision: P20 at 10:00 + P100 at 10:05 in one
# paid period is ONE row of P120, stamped at the first billing.
r = Ring()
r.record(12000, ts=1789696800)     # P120 billed across one session
evs = r.pull(0)
ok &= check("P20 + P100 in one session -> 1 row, P120, stamped 10:00",
            len(evs) == 1 and evs[0]['amount'] == 120 and evs[0]['ts'] == 1789696800)

print("\n" + ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED"))
