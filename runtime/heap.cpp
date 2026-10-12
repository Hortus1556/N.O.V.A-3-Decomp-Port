// Guest heap: malloc/free/calloc/realloc returning guest addresses.
//
// Every block has a 16-byte header (so user pointers are 16-aligned):
//   +0 capacity (bytes usable by the caller)   +4 size class (kLarge for page blocks)
// Small requests (<= 64 KiB) use segregated free lists per size class; larger ones are
// page-granular with best-fit reuse and coalescing. All memory comes from one bump
// region [kHeapBase, kHeapEnd) that is committed in 32 MiB steps.
//
// NOVA3_HEAP_PROFILE=N (Windows): every N frames the log gets the live bytes by the guest call
// stack that allocated them (`heap profile:` lines, the stacks' innermost guest functions
// first), to see what fills the heap (measured for small-memory ports). NOVA3_HEAP_DUMP=F writes
// the heap's bytes at frame F to heap_F.bin (its compressibility, for paging to compressed memory).
#include "os.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
namespace {

constexpr uint32_t kHeader = 16;
constexpr uint32_t kLarge = 0xffffffffu;
constexpr uint32_t kSmallMax = 65536;
constexpr uint32_t kCommitStep = 32u << 20;

std::mutex g_lock;
uint32_t g_top = kHeapBase, g_committed = kHeapBase;
uint64_t g_live, g_peak;  // bytes handed out (capacities), and the most at once
std::vector<uint32_t> g_class_size;    // capacity of each small class
std::vector<uint32_t> g_free_head;     // free list head per class (guest address of header)
std::map<uint32_t, uint32_t> g_large_free;            // header addr -> total block bytes
std::multimap<uint32_t, uint32_t> g_large_by_size;    // total bytes -> header addr

// ---- the heap profile (NOVA3_HEAP_PROFILE) ----
constexpr size_t kProfileDepth = 40;
const uint64_t g_profile_every = [] { const char *e = std::getenv("NOVA3_HEAP_PROFILE"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
uint64_t g_profile_next = g_profile_every;
struct StackKey {
    std::array<void *, kProfileDepth> pcs{};
    bool operator==(const StackKey &o) const { return pcs == o.pcs; }
};
struct StackHash {
    size_t operator()(const StackKey &k) const {
        size_t h = 0;
        for (void *p : k.pcs) h = h * 1000003u ^ size_t(p);
        return h;
    }
};
std::unordered_map<StackKey, uint32_t, StackHash> g_stack_ids;
std::vector<StackKey> g_stacks;
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_blocks;  // user address -> (bytes, stack id)

void profile_report();

const uint64_t g_dump_frame = [] { const char *e = std::getenv("NOVA3_HEAP_DUMP"); return e ? std::strtoull(e, nullptr, 10) : 0; }();

void dump_heap() {
    char name[64];
    std::snprintf(name, sizeof name, "heap_%llu.bin", (unsigned long long)watchdog_frames());
    if (FILE *f = std::fopen(name, "wb")) {
        std::fwrite(ptr(kHeapBase), 1, g_top - kHeapBase, f);
        std::fclose(f);
        log("heap: %u MB written to %s", (g_top - kHeapBase) >> 20, name);
    }
}

void profile_alloc(uint32_t p, uint32_t bytes) {
    StackKey k;
    os::capture_stack(k.pcs.data(), kProfileDepth);
    auto [it, added] = g_stack_ids.emplace(k, uint32_t(g_stacks.size()));
    if (added) g_stacks.push_back(k);
    g_blocks[p] = {bytes, it->second};
    if (watchdog_frames() >= g_profile_next) {
        g_profile_next = watchdog_frames() + g_profile_every;
        profile_report();
    }
}

// The guest functions of a stack, innermost first (recompiled functions are f_/t_<link address>).
std::string guest_chain(const StackKey &k, int depth) {
    static std::unordered_map<void *, std::string> names;
    std::string out;
    int n = 0;
    for (void *pc : k.pcs) {
        if (!pc || n == depth) break;
        auto it = names.find(pc);
        if (it == names.end()) {
            char name[256];
            std::string g;
            if (os::host_symbol(pc, name, sizeof name) && (name[0] == 'f' || name[0] == 't') && name[1] == '_') {
                g = guest_symbol(uint32_t(std::strtoul(name + 2, nullptr, 16)));
                g = g.substr(0, g.find('+'));
            }
            it = names.emplace(pc, g).first;
        }
        if (it->second.empty() || (n && out.size() >= it->second.size() &&
                                   out.compare(out.size() - it->second.size(), it->second.size(), it->second) == 0))
            continue;  // host frames, and a function's frames repeated (the same function at another pc)
        out += n++ ? " < " : "";
        out += it->second;
    }
    return out;
}

void profile_report() {
    std::unordered_map<std::string, std::pair<uint64_t, uint32_t>> by_chain;  // bytes, blocks
    uint64_t total = 0;
    for (auto &[p, b] : g_blocks) {
        auto &c = by_chain[guest_chain(g_stacks[b.second], 6)];
        c.first += b.first, c.second++, total += b.first;
    }
    std::vector<std::pair<std::string, std::pair<uint64_t, uint32_t>>> v(by_chain.begin(), by_chain.end());
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.first > b.second.first; });
    log("heap profile: frame %llu, %.1f MB live in %zu blocks (peak %.1f MB)", (unsigned long long)watchdog_frames(),
        total / 1048576.0, g_blocks.size(), g_peak / 1048576.0);
    for (size_t i = 0; i < v.size() && i < 60 && v[i].second.first >= 65536; i++)
        log("heap profile: %7.2f MB %6u  %s", v[i].second.first / 1048576.0, v[i].second.second, v[i].first.c_str());
}

void init_classes() {
    if (!g_class_size.empty()) return;
    for (uint32_t s = 16; s <= 256; s += 16) g_class_size.push_back(s);
    for (uint32_t base = 256; base < kSmallMax; base *= 2)
        for (uint32_t k = 1; k <= 4; k++) g_class_size.push_back(base + base / 4 * k);
    g_free_head.assign(g_class_size.size(), 0);
}

uint32_t class_of(uint32_t size) {
    // lower_bound over a short sorted vector
    uint32_t lo = 0, hi = uint32_t(g_class_size.size());
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_class_size[mid] < size) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

uint32_t bump(uint32_t bytes) {
    if (uint64_t(g_top) + bytes > kHeapEnd) fatal("guest heap exhausted (%u MiB in use)", (g_top - kHeapBase) >> 20);
    uint32_t a = g_top;
    g_top += bytes;
    while (g_top > g_committed) {
        commit(g_committed, kCommitStep);
        g_committed += kCommitStep;
    }
    return a;
}

void large_remove(uint32_t addr, uint32_t size) {
    g_large_free.erase(addr);
    auto range = g_large_by_size.equal_range(size);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == addr) { g_large_by_size.erase(it); break; }
}

void large_insert(uint32_t addr, uint32_t size) {
    // coalesce with neighbours
    auto next = g_large_free.find(addr + size);
    if (next != g_large_free.end()) {
        uint32_t ns = next->second;
        large_remove(addr + size, ns);
        size += ns;
    }
    auto prev = g_large_free.lower_bound(addr);
    if (prev != g_large_free.begin()) {
        --prev;
        if (prev->first + prev->second == addr) {
            uint32_t pa = prev->first, ps = prev->second;
            large_remove(pa, ps);
            addr = pa;
            size += ps;
        }
    }
    g_large_free[addr] = size;
    g_large_by_size.emplace(size, addr);
}

uint32_t alloc_locked(uint32_t size) {
    init_classes();
    if (g_dump_frame && watchdog_frames() == g_dump_frame) {
        static bool done;
        if (!done) done = true, dump_heap();
    }
    if (size == 0) size = 1;
    if (size <= kSmallMax) {
        uint32_t c = class_of(size);
        uint32_t h = g_free_head[c];
        if (h) {
            g_free_head[c] = LD32(h + kHeader);
        } else {
            h = bump(kHeader + g_class_size[c]);
            ST32(h, g_class_size[c]);
            ST32(h + 4, c);
        }
        g_live += g_class_size[c];
        g_peak = std::max(g_peak, g_live);
        if (g_profile_every) profile_alloc(h + kHeader, g_class_size[c]);
        return h + kHeader;
    }
    uint32_t total = (size + kHeader + 0xfff) & ~0xfffu;
    auto it = g_large_by_size.lower_bound(total);
    uint32_t h;
    if (it != g_large_by_size.end()) {
        h = it->second;
        uint32_t have = it->first;
        large_remove(h, have);
        if (have - total >= kSmallMax) large_insert(h + total, have - total);
        else total = have;
    } else {
        h = bump(total);
    }
    ST32(h, total - kHeader);
    ST32(h + 4, kLarge);
    g_live += total - kHeader;
    g_peak = std::max(g_peak, g_live);
    if (g_profile_every) profile_alloc(h + kHeader, total - kHeader);
    return h + kHeader;
}

void free_locked(uint32_t p) {
    if (g_profile_every) g_blocks.erase(p);
    uint32_t h = p - kHeader;
    uint32_t c = LD32(h + 4);
    g_live -= c == kLarge ? LD32(h) : g_class_size[c < g_class_size.size() ? c : 0];
    if (c == kLarge) {
        const uint32_t total = LD32(h) + kHeader;
        large_insert(h, total);
        // its whole pages (past the header) need not be kept: the GameCube's pager drops them
        const uint32_t lo = (h + kHeader + 0xfff) & ~0xfffu, hi = (h + total) & ~0xfffu;
        if (hi > lo) os::discard(ptr(lo), hi - lo);
    } else {
        if (c >= g_class_size.size()) fatal("free(%08x): corrupt heap header", p);
        ST32(p, g_free_head[c]);
        g_free_head[c] = h;
    }
}

}  // namespace

bool heap_committed(uint32_t addr) {
    std::lock_guard<std::mutex> lock(g_lock);
    return addr >= kHeapBase && addr < g_committed;
}

void heap_usage(uint64_t &live, uint64_t &peak, uint64_t &span) {
    std::lock_guard<std::mutex> lock(g_lock);
    live = g_live, peak = g_peak, span = g_top - kHeapBase;
}

uint32_t gmalloc(uint32_t size) {
    std::lock_guard<std::mutex> lock(g_lock);
    return alloc_locked(size);
}

void gfree(uint32_t p) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(g_lock);
    free_locked(p);
}

uint32_t gcalloc(uint32_t n, uint32_t size) {
    uint64_t total = uint64_t(n) * size;
    if (total > 0xffffffffull) { set_errno(kENOMEM); return 0; }
    uint32_t p = gmalloc(uint32_t(total));
    std::memset(ptr(p), 0, size_t(total));
    return p;
}

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return gmalloc(size);
    if (!size) { gfree(p); return 0; }
    uint32_t cap = LD32(p - kHeader);
    if (size <= cap) return p;
    uint32_t q = gmalloc(size);
    std::memcpy(ptr(q), ptr(p), cap);
    gfree(p);
    return q;
}

uint32_t gstrdup(const char *s) {
    size_t n = std::strlen(s) + 1;
    uint32_t p = gmalloc(uint32_t(n));
    std::memcpy(ptr(p), s, n);
    return p;
}

RT_IMPORT(malloc, gmalloc)
RT_IMPORT(free, gfree)
RT_IMPORT(calloc, gcalloc)
RT_IMPORT(realloc, grealloc)

}  // namespace rt
