// Rewind: while the button is held the game goes back through the states of the last 10 seconds (one
// every second frame) at twice the speed; let go and it plays on from there. A restored state runs
// two frames, so the frame it draws is shown (the game shows a frame at the VSync after it).
//
// The states are kept as differences: `g_cur` is the newest whole state and each entry is what
// changed from the one before (XOR, runs of zeros left out), so going back one is cur ^= entry.
// States differ in size too (the SPU's CD audio queue), so an entry starts with the size of the
// state before.
#include "core/psx.h"
#include "core/savestate.h"
#include "host/host.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

bool g_rewind = true;
bool g_rewind_used = false;

namespace {

constexpr uint64_t kEvery = 2, kKeep = 10 * 60;  // frames

struct Entry {
    std::vector<uint8_t> bytes;
    uint64_t frame = 0;
};
std::deque<Entry> g_ring;
std::vector<uint8_t> g_cur, g_next, g_spare;  // (g_spare: a dropped entry's memory, reused)
uint64_t g_captured_frame = 0;
bool g_active = false;
uint64_t g_restored_frame = ~0ull;

void put32(std::vector<uint8_t>& out, uint32_t v) { out.insert(out.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }

// The way from state a back to state b: [b's size, u32], then runs over b of
// [bytes to skip, u32][bytes that differ, u32][those bytes of a ^ b] (a past its end: zeros).
// (fewer than 8 equal bytes stay inside a run: a new run costs 8)
void encode(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::vector<uint8_t>& out) {
    put32(out, (uint32_t)b.size());
    size_t last = 0;
    auto run = [&](size_t start, size_t end) {
        put32(out, (uint32_t)(start - last));
        put32(out, (uint32_t)(end - start));
        const size_t at = out.size();
        out.resize(at + (end - start));
        for (size_t k = start; k < end; k++) out[at + k - start] = (k < a.size() ? a[k] : 0) ^ b[k];
        last = end;
    };
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n;) {
        while (i + 8 <= n && std::memcmp(&a[i], &b[i], 8) == 0) i += 8;
        while (i < n && a[i] == b[i]) i++;
        if (i == n) break;
        const size_t start = i;
        size_t same = 0;
        for (; i < n && same < 8; i++) same = a[i] == b[i] ? same + 1 : 0;
        run(start, i - same);
        i = last;
    }
    if (b.size() > n) run(n, b.size());
}

void undo(std::vector<uint8_t>& cur, const std::vector<uint8_t>& runs) {
    uint32_t size;
    std::memcpy(&size, runs.data(), 4);
    cur.resize(size);
    size_t pos = 0;
    for (size_t p = 4; p + 8 <= runs.size();) {
        uint32_t skip, len;
        std::memcpy(&skip, &runs[p], 4);
        std::memcpy(&len, &runs[p + 4], 4);
        p += 8;
        pos += skip;
        for (uint32_t k = 0; k < len; k++) cur[pos + k] ^= runs[p + k];
        p += len;
        pos += len;
    }
}

void capture(R3000* c, uint32_t pc, uint64_t frame) {
    state_capture(g_next, c, pc);
    if (!g_cur.empty()) {
        Entry e{std::move(g_spare), frame};
        e.bytes.clear();
        encode(g_next, g_cur, e.bytes);
        g_ring.push_back(std::move(e));
        while (g_ring.front().frame + kKeep < frame) {
            g_spare = std::move(g_ring.front().bytes);
            g_ring.pop_front();
        }
    }
    g_cur.swap(g_next);
}

}  // namespace

bool rewind_available() { return g_rewind && !g_ring.empty(); }
bool rewind_active() { return g_active; }

void rewind_clear() {
    g_ring.clear();
    g_cur.clear();
    g_active = false;
}

void rewind_frame(R3000* c, uint32_t pc, bool allowed) {
    const uint64_t frame = host_frame_count();
    if (frame == g_restored_frame) return;  // (VSync again: the restored state resumes at its entry)
    if (!g_rewind) {
        if (!g_cur.empty()) rewind_clear();
        return;
    }
    if (!allowed) {
        g_active = false;
        return;
    }
    const bool held = host_rewind_held() && !g_cur.empty();
    if (held && frame == g_restored_frame + 1) return;
    if (held) {
        g_active = g_rewind_used = true;
        for (int i = 0; i < 2 && !g_ring.empty(); i++) {  // (none left: the oldest state again, held there)
            Entry& e = g_ring.back();
            undo(g_cur, e.bytes);
            g_spare = std::move(e.bytes);
            g_ring.pop_back();
        }
        g_restored_frame = frame;
        state_restore(g_cur, c);
    }
    g_active = false;
    if (frame < g_captured_frame + kEvery) return;
    g_captured_frame = frame;
    capture(c, pc, frame);
}
