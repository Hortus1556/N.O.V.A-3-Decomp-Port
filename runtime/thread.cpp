// pthreads / semaphores / errno on portable C++ sync objects and rt::os threads.
//
// Each guest thread has its own Cpu and guest stack and runs on a host thread with a
// large stack reservation (every guest call nests a host call). bionic's mutex, cond
// and sem objects are single 32-bit guest words; on first use we install a handle
// (kHandleTag | index into g_sync) in that word with a CAS, so statically initialised
// objects (PTHREAD_MUTEX_INITIALIZER etc.) work without a lookup table. A handle counts
// only in the word it was installed in (the object notes that address): a word that merely
// looks like one (memory the game never initialised, an object copied elsewhere, a word
// whose object was freed without being destroyed) is a fresh object, and destroying it
// releases nothing. Trusting any tagged word, a stray one's destroy deleted another live
// object while its threads still used it.
#include "os.h"
#include "tls.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rt {
namespace {

constexpr uint32_t kHandleTag = 0x40000000u;
#ifdef __3DS__
constexpr uint32_t kMaxSync = 1u << 18;  // live guest mutexes / conditions / semaphores (the table: 1 MB)
#elif defined(__GAMECUBE__)
constexpr uint32_t kMaxSync = 1u << 16;  // (the table: 256 KB of MEM1)
#else
constexpr uint32_t kMaxSync = 1u << 20;
#endif
constexpr uint32_t kMaxKeys = 128;
constexpr uint32_t kDefaultGuestStack = 1u << 20;
constexpr size_t kHostStackReserve = size_t(256) << 20;

struct GuestThread {
    Cpu *cpu = nullptr;
    uint32_t host_id = 0;       // os::thread_id() of the thread running it (hang reports)
    uint32_t waiting_for = 0;   // the guest mutex it is blocked on, while it is (hang reports)
    GuestThread *joining = nullptr;  // the thread it waits for in pthread_join, while it does
    uint32_t self = 0;          // guest-visible pthread_t
    uint32_t errno_addr = 0;
    uint32_t tls = 0;           // guest TLS block (__kuser_get_tls)
    uint32_t stack = 0;         // guest stack block (heap; none on the 3DS, whose game code uses the host stack)
    uint32_t start = 0, arg = 0, ret = 0;  // start: 0 for the main thread
    uint32_t keys[kMaxKeys] = {};
    bool detached = false;      // guarded by g_threads_lock
    bool finished = false;      // guarded by g_threads_lock (pthread_join polls it)
    bool joined = false;        // guarded by g_threads_lock: a pthread_join claimed it
    jmp_buf exit_buf;           // setjmp target for pthread_exit (C library pair, see recomp.h)
};

RT_THREAD_LOCAL(GuestThread *, t_cur);
Cpu *g_main_cpu;  // the game thread's (watchdog reports)
std::mutex g_threads_lock;
std::unordered_map<uint32_t, GuestThread *> g_threads;

// All guest mutexes are recursive (bionic's default and recursive types both work).
struct SyncObj {
    enum Kind { Mutex, Cond, Sem } kind;
    uint32_t addr = 0;             // the guest word holding its handle
    std::recursive_mutex mutex;
    GuestThread *owner = nullptr;  // a mutex's holder and its lock count (hang reports name the holder)
    uint32_t depth = 0;
    std::condition_variable_any cv;
    std::mutex sem_lock;  // semaphore: count guarded by sem_lock, waiters on cv
    uint32_t sem_count = 0;
};
SyncObj *g_sync[kMaxSync];
std::atomic<uint32_t> g_sync_next{1};
std::mutex g_sync_free_lock;
std::vector<uint32_t> g_sync_free;

uint32_t sync_alloc(SyncObj *o) {
    uint32_t i = 0;
    {
        std::lock_guard<std::mutex> lock(g_sync_free_lock);
        if (!g_sync_free.empty()) { i = g_sync_free.back(); g_sync_free.pop_back(); }
    }
    if (!i) i = g_sync_next++;
    if (i >= kMaxSync) fatal("too many guest sync objects");
    g_sync[i] = o;
    return i;
}

SyncObj *make(SyncObj::Kind kind, uint32_t addr, uint32_t sem_value = 0) {
    auto *o = new SyncObj;
    o->kind = kind;
    o->addr = addr;
    o->sem_count = sem_value;
    return o;
}

void destroy(SyncObj *o) { delete o; }

// The object a word's handle names, if the handle was installed in that word
SyncObj *installed(uint32_t addr, uint32_t v) {
    if (!(v & kHandleTag)) return nullptr;
    SyncObj *o = g_sync[v & (kMaxSync - 1)];
    return o && o->addr == addr ? o : nullptr;
}
// the first stray handles met (a word that looks like a handle but is not this word's), logged
void note_stray(const char *what, uint32_t addr, uint32_t v) {
    static std::atomic<int> logged{0};
    if (logged++ < 8) log("thread: %s of %08x, which holds %08x: not its handle (a fresh object)", what, addr, v);
}

// Resolve (lazily creating) the host object behind a guest sync word.
SyncObj *lookup(uint32_t addr, SyncObj::Kind kind) {
    auto *word = reinterpret_cast<std::atomic<uint32_t> *>(ptr(addr));
    uint32_t v = word->load(std::memory_order_acquire);
    if (SyncObj *o = installed(addr, v)) return o;
    if (v & kHandleTag) note_stray("a lookup", addr, v);
    for (;;) {
        SyncObj *o = make(kind, addr);
        uint32_t idx = sync_alloc(o);
        if (word->compare_exchange_strong(v, kHandleTag | idx)) return o;
        // another thread installed one first (v: its handle)
        destroy(o);
        g_sync[idx] = nullptr;
        { std::lock_guard<std::mutex> lock(g_sync_free_lock); g_sync_free.push_back(idx); }
        if (SyncObj *w = installed(addr, v)) return w;
    }
}

void release(uint32_t addr) {
    uint32_t v = LD32(addr);
    SyncObj *o = installed(addr, v);
    if (!o) {
        if (v & kHandleTag) note_stray("a destroy", addr, v);
        return;
    }
    uint32_t idx = v & (kMaxSync - 1);
    ST32(addr, 0);
    g_sync[idx] = nullptr;
    destroy(o);
    { std::lock_guard<std::mutex> lock(g_sync_free_lock); g_sync_free.push_back(idx); }
}

struct Key {
    bool used = false;
    uint32_t destructor = 0;
};
Key g_keys[kMaxKeys];
std::mutex g_keys_lock;

GuestThread *new_guest_thread() {
    auto *t = new GuestThread;
    t->self = gcalloc(1, 64);
    t->tls = gcalloc(64, 4);         // bionic TLS slots: [0] self, [1] thread id, [2] errno
    ST32(t->tls, t->tls);
    t->errno_addr = t->tls + 8;
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads[t->self] = t;
    return t;
}

void run_key_destructors(GuestThread *t) {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (uint32_t k = 0; k < kMaxKeys; k++) {
            uint32_t value = t->keys[k], dtor;
            {
                std::lock_guard<std::mutex> lock(g_keys_lock);
                dtor = g_keys[k].used ? g_keys[k].destructor : 0;
            }
            if (value && dtor) {
                t->keys[k] = 0;
                call(t->cpu, dtor, {value});
                any = true;
            }
        }
        if (!any) break;
    }
}

// What a thread uses while it runs: its stack and registers (an ended thread keeps only what
// pthread_join reads).
void free_running_state(GuestThread *t) {
    if (t->stack) gfree(t->stack);
    t->stack = 0;
    note_guest_stack(t->cpu, 0);
    std::free(t->cpu);
    t->cpu = nullptr;
}

// pthread_t values are guest blocks (bionic's are its thread structures). A freed block is the
// next thread's at once, so a stale handle (a second join, or a detach, of a thread that is gone)
// would name a live thread: a released one waits behind the last 1024 before it is freed.
std::deque<uint32_t> g_released_selves;  // guarded by g_threads_lock

// Frees an ended thread that is out of g_threads (no pthread_join or pthread_detach finds it).
void discard_thread(GuestThread *t) {
    free_running_state(t);
    gfree(t->tls);
    uint32_t self_to_free = 0;
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        g_released_selves.push_back(t->self);
        if (g_released_selves.size() > 1024) {
            self_to_free = g_released_selves.front();
            g_released_selves.pop_front();
        }
    }
    if (self_to_free) gfree(self_to_free);
    delete t;
}
void forget_thread(GuestThread *t) {
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        g_threads.erase(t->self);
    }
    discard_thread(t);
}

void thread_main(void *param) {
    auto *t = static_cast<GuestThread *>(param);
    RT_TLS(t_cur) = t;
    t->host_id = uint32_t(os::thread_id());
    if (setjmp(t->exit_buf) == 0) t->ret = call(t->cpu, t->start, {t->arg});
    run_key_destructors(t);
    free_running_state(t);  // now, even if the game never joins it
    bool detached;
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        detached = t->detached;
        t->finished = true;
        if (detached) g_threads.erase(t->self);
    }
    if (detached) discard_thread(t);
}

// ---- pthread_attr / mutexattr (bionic layouts) -------------------------------------
// pthread_attr_t: flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
int attr_init(uint32_t a) {
    const uint32_t v[6] = {0, 0, kDefaultGuestStack, 4096, 0, 0};
    for (uint32_t i = 0; i < 6; i++) ST32(a + 4 * i, v[i]);
    return 0;
}
int attr_destroy(uint32_t) { return 0; }
int attr_setdetachstate(uint32_t a, int s) { ST32(a, s ? LD32(a) | 1 : LD32(a) & ~1u); return 0; }
int attr_getdetachstate(uint32_t a, uint32_t out) { ST32(out, LD32(a) & 1); return 0; }
int attr_setstacksize(uint32_t a, uint32_t size) { ST32(a + 8, size); return 0; }
int attr_setschedparam(uint32_t a, uint32_t param) { ST32(a + 20, LD32(param)); return 0; }
int attr_setschedpolicy(uint32_t a, int p) { ST32(a + 16, uint32_t(p)); return 0; }
int attr_setscope(uint32_t, int) { return 0; }

int mutexattr_init(uint32_t a) { ST32(a, 0); return 0; }
int mutexattr_destroy(uint32_t) { return 0; }
int mutexattr_settype(uint32_t a, int type) { ST32(a, uint32_t(type)); return 0; }

// ---- threads ---------------------------------------------------------------------------
int p_create(Cpu *cpu, uint32_t out, uint32_t attr, uint32_t start, uint32_t arg) {
    // NOVA3_TRACE_THREADS=1: which code starts which thread (engine threads share a start
    // routine: the creator tells); off by default (the web services start threads every few seconds)
    static const bool trace = [] { const char *v = std::getenv("NOVA3_TRACE_THREADS"); return v && *v == '1'; }();
    if (trace) {
        log("thread: start %08x, created from %08x", start - g_load_base, cpu->r[14] - g_load_base);
        log_guest_callers(cpu);
    }
    uint32_t stack_size = attr ? std::max<uint32_t>(LD32(attr + 8), 64u << 10) : kDefaultGuestStack;
    GuestThread *t = new_guest_thread();
    t->start = start;
    t->arg = arg;
    t->detached = attr && (LD32(attr) & 1);
#ifdef __3DS__
    // native code (runtime/native_3ds.cpp) runs on the host thread's stack: the size the game
    // asked for, plus room for the runtime's own frames under the imports
    t->cpu = new_cpu(stack_size);
    const size_t host_stack = size_t(stack_size) + (64u << 10);
#else
    t->stack = gmalloc(stack_size);
    t->cpu = static_cast<Cpu *>(std::calloc(1, sizeof(Cpu)));
    t->cpu->r[13] = (t->stack + stack_size - 16) & ~15u;
    note_guest_stack(t->cpu, t->stack + stack_size);
    t->cpu->ex_addr = ~0u;
    const size_t host_stack = kHostStackReserve;
#endif
    ST32(out, t->self);
    {
        // many threads at once: a sign of threads that never end; report them at 40, 80, ...
        static int reported = 0;
        int running, unjoined;
        thread_counts(running, unjoined);
        if (running >= 40 && running / 40 > reported) {
            reported = running / 40;
            log("thread: %d guest threads running (%d ended, not joined); new one %08x from %08x", running, unjoined,
                start - g_load_base, cpu->r[14] - g_load_base);
            log_flush();
            std::lock_guard<std::mutex> lock(report_lock());
            report_threads(stderr);
            std::fflush(stderr);
        }
    }
    if (!os::start_thread(thread_main, t, host_stack)) {
        log("pthread_create: no thread for %08x (stack %u KB)", start - g_load_base, stack_size >> 10);
        forget_thread(t);
        return kEAGAIN;
    }
    return 0;  // a detached thread frees its GuestThread when it ends
}

// As bionic: a thread is joined once and never used after it is freed (the lookup, the claim and
// the removal under the threads' lock); a second join, or a join of a detached thread, fails at
// once (bionic: EINVAL). The wait polls the thread's end every 2 ms instead of sleeping on a
// condition variable: on the 3DS a join sat forever on the condition variable of a thread that
// had ended long before (the glwebtools update thread, holding its core mutex, joining an idle
// pool thread: the title-menu hang).
int p_join(uint32_t id, uint32_t retval) {
    GuestThread *t;
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        auto it = g_threads.find(id);
        if (it == g_threads.end()) return 3;  // ESRCH
        t = it->second;
        if (t == RT_TLS(t_cur)) return kEDEADLK;
        if (t->detached || t->joined) return kEINVAL;
        t->joined = true;
        if (RT_TLS(t_cur)) RT_TLS(t_cur)->joining = t;
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_threads_lock);
            if (t->finished) {
                g_threads.erase(id);
                break;
            }
        }
        os::sleep_ns(2'000'000);
    }
    if (RT_TLS(t_cur)) RT_TLS(t_cur)->joining = nullptr;
    if (retval) ST32(retval, t->ret);
    discard_thread(t);
    return 0;
}

int p_detach(uint32_t id) {
    GuestThread *t;
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        auto it = g_threads.find(id);
        if (it == g_threads.end()) return 3;  // ESRCH
        t = it->second;
        if (t->detached || t->joined) return kEINVAL;
        if (!t->finished) {
            t->detached = true;  // thread_main frees it when it ends
            return 0;
        }
        g_threads.erase(id);
    }
    discard_thread(t);
    return 0;
}

void p_exit(uint32_t value) {
    GuestThread *t = RT_TLS(t_cur);
    if (!t->start) { log("pthread_exit on main thread"); os::exit_process(0); }
    t->ret = value;
    longjmp(t->exit_buf, 1);
}

uint32_t p_self() { return RT_TLS(t_cur)->self; }
int p_equal(uint32_t a, uint32_t b) { return a == b; }
int p_kill(uint32_t, int) { return 0; }
int p_getschedparam(uint32_t, uint32_t policy, uint32_t param) {
    if (policy) ST32(policy, 0);
    if (param) ST32(param, 0);
    return 0;
}
int p_setschedparam(uint32_t, int, uint32_t) { return 0; }
int sched_prio_max(int policy) { return policy ? 99 : 0; }
int sched_prio_min(int policy) { return policy ? 1 : 0; }
int p_sched_yield() { os::yield(); return 0; }

// ---- keys / once ------------------------------------------------------------------------
int key_create(uint32_t out, uint32_t destructor) {
    std::lock_guard<std::mutex> lock(g_keys_lock);
    for (uint32_t k = 0; k < kMaxKeys; k++) {
        if (!g_keys[k].used) {
            g_keys[k] = {true, destructor};
            ST32(out, k);
            return 0;
        }
    }
    return kEAGAIN;
}
int key_delete(uint32_t k) {
    if (k >= kMaxKeys) return kEINVAL;
    std::lock_guard<std::mutex> lock(g_keys_lock);
    g_keys[k] = {};
    return 0;
}
int set_specific(uint32_t k, uint32_t v) {
    if (k >= kMaxKeys) return kEINVAL;
    RT_TLS(t_cur)->keys[k] = v;
    return 0;
}
uint32_t get_specific(uint32_t k) { return k < kMaxKeys ? RT_TLS(t_cur)->keys[k] : 0; }

int p_once(Cpu *cpu, uint32_t once, uint32_t fn) {
    auto *word = reinterpret_cast<std::atomic<uint32_t> *>(ptr(once));
    uint32_t expected = 0;
    if (word->compare_exchange_strong(expected, 1)) {
        call_nested(cpu, fn, {});
        word->store(2);
        return 0;
    }
    while (word->load() != 2) os::yield();
    return 0;
}

// ---- mutex / cond / sem --------------------------------------------------------------------
int mutex_init(uint32_t m, uint32_t attr) {
    (void)attr;  // all host mutexes are recursive critical sections
    ST32(m, 0);
    lookup(m, SyncObj::Mutex);
    return 0;
}
int mutex_destroy(uint32_t m) { release(m); return 0; }
// the holder is noted once the lock is taken and cleared before the last unlock (only the
// holder writes these)
void took(SyncObj *o) {
    o->owner = RT_TLS(t_cur);
    o->depth++;
}
void leaving(SyncObj *o) {
    if (o->depth && !--o->depth) o->owner = nullptr;
}
int mutex_lock(uint32_t m) {
    SyncObj *o = lookup(m, SyncObj::Mutex);
    if (!o->mutex.try_lock()) {
        if (RT_TLS(t_cur)) RT_TLS(t_cur)->waiting_for = m;
        o->mutex.lock();
        if (RT_TLS(t_cur)) RT_TLS(t_cur)->waiting_for = 0;
    }
    took(o);
    return 0;
}
int mutex_trylock(uint32_t m) {
    SyncObj *o = lookup(m, SyncObj::Mutex);
    if (!o->mutex.try_lock()) return kEBUSY;
    took(o);
    return 0;
}
int mutex_unlock(uint32_t m) {
    SyncObj *o = lookup(m, SyncObj::Mutex);
    leaving(o);
    o->mutex.unlock();
    return 0;
}

int cond_init(uint32_t c, uint32_t) { ST32(c, 0); lookup(c, SyncObj::Cond); return 0; }
int cond_destroy(uint32_t c) { release(c); return 0; }
int cond_signal(uint32_t c) { lookup(c, SyncObj::Cond)->cv.notify_one(); return 0; }
int cond_broadcast(uint32_t c) { lookup(c, SyncObj::Cond)->cv.notify_all(); return 0; }
// A wait releases the mutex once (a recursive one stays held at a deeper count) and takes it back.
// On the 3DS a wait also ends after kCondSlice, as a spurious wakeup (which POSIX allows: the
// guest checks its condition again): a wakeup lost in the console's condition variables, as a
// pthread_join's was, then costs a moment instead of the game.
#ifdef __3DS__
constexpr int64_t kCondSlice = 100'000'000;
#else
constexpr int64_t kCondSlice = INT64_MAX;
#endif
int cond_wait(uint32_t c, uint32_t m) {
    SyncObj *o = lookup(m, SyncObj::Mutex);
    const uint32_t depth = o->depth;
    leaving(o);
    if (kCondSlice == INT64_MAX) lookup(c, SyncObj::Cond)->cv.wait(o->mutex);
    else lookup(c, SyncObj::Cond)->cv.wait_for(o->mutex, std::chrono::nanoseconds(kCondSlice));
    o->owner = RT_TLS(t_cur), o->depth = depth;
    return 0;
}
int cond_timedwait(uint32_t c, uint32_t m, uint32_t abstime) {
    // abstime is CLOCK_REALTIME (timespec: 32-bit sec, nsec): wait for the remaining time
    int64_t target = int64_t(int32_t(LD32(abstime))) * 1000000000 + LD32(abstime + 4);
    int64_t left = std::max<int64_t>(0, target - os::realtime_ns());
    SyncObj *o = lookup(m, SyncObj::Mutex);
    const uint32_t depth = o->depth;
    leaving(o);
    auto status = lookup(c, SyncObj::Cond)->cv.wait_for(o->mutex, std::chrono::nanoseconds(std::min(left, kCondSlice)));
    o->owner = RT_TLS(t_cur), o->depth = depth;
    return status == std::cv_status::timeout && left <= kCondSlice ? kETIMEDOUT : 0;
}

int sem_init_(uint32_t s, int, uint32_t value) {
    SyncObj *o = make(SyncObj::Sem, s, value);
    ST32(s, kHandleTag | sync_alloc(o));
    return 0;
}
int sem_destroy_(uint32_t s) { release(s); return 0; }
int sem_wait_(uint32_t s) {
    SyncObj *o = lookup(s, SyncObj::Sem);
    std::unique_lock<std::mutex> lock(o->sem_lock);
    while (o->sem_count == 0) {  // (in slices on the 3DS, as cond_wait)
        if (kCondSlice == INT64_MAX) o->cv.wait(lock);
        else o->cv.wait_for(lock, std::chrono::nanoseconds(kCondSlice));
    }
    o->sem_count--;
    return 0;
}
int sem_trywait_(uint32_t s) {
    SyncObj *o = lookup(s, SyncObj::Sem);
    std::lock_guard<std::mutex> lock(o->sem_lock);
    if (o->sem_count > 0) { o->sem_count--; return 0; }
    set_errno(kEAGAIN);
    return -1;
}
int sem_post_(uint32_t s) {
    SyncObj *o = lookup(s, SyncObj::Sem);
    { std::lock_guard<std::mutex> lock(o->sem_lock); o->sem_count++; }
    o->cv.notify_one();
    return 0;
}

uint32_t errno_location() { return RT_TLS(t_cur)->errno_addr; }

}  // namespace

Cpu *init_main_thread() {
    GuestThread *t = new_guest_thread();
    t->cpu = new_cpu(8u << 20);
    t->host_id = uint32_t(os::thread_id());
    RT_TLS(t_cur) = t;
    g_main_cpu = t->cpu;
    return t->cpu;
}

Cpu *current_cpu() { return RT_TLS(t_cur) ? RT_TLS(t_cur)->cpu : nullptr; }
uint32_t current_tls() { return RT_TLS(t_cur) ? RT_TLS(t_cur)->tls : 0; }
void set_errno(int e) { if (RT_TLS(t_cur)) ST32(RT_TLS(t_cur)->errno_addr, uint32_t(e)); }
int get_errno() { return RT_TLS(t_cur) ? int(LD32(RT_TLS(t_cur)->errno_addr)) : 0; }
uint32_t current_thread_id() { return uint32_t(os::thread_id()); }

RT_IMPORT(pthread_create, p_create)
RT_IMPORT(pthread_join, p_join)
RT_IMPORT(pthread_detach, p_detach)
RT_IMPORT(pthread_exit, p_exit)
RT_IMPORT(pthread_self, p_self)
RT_IMPORT(pthread_equal, p_equal)
RT_IMPORT(pthread_kill, p_kill)
RT_IMPORT(pthread_getschedparam, p_getschedparam)
RT_IMPORT(pthread_setschedparam, p_setschedparam)
RT_IMPORT(sched_get_priority_max, sched_prio_max)
RT_IMPORT(sched_get_priority_min, sched_prio_min)
RT_IMPORT(sched_yield, p_sched_yield)
RT_IMPORT(pthread_attr_init, attr_init)
RT_IMPORT(pthread_attr_destroy, attr_destroy)
RT_IMPORT(pthread_attr_setdetachstate, attr_setdetachstate)
RT_IMPORT(pthread_attr_getdetachstate, attr_getdetachstate)
RT_IMPORT(pthread_attr_setstacksize, attr_setstacksize)
RT_IMPORT(pthread_attr_setschedparam, attr_setschedparam)
RT_IMPORT(pthread_attr_setschedpolicy, attr_setschedpolicy)
RT_IMPORT(pthread_attr_setscope, attr_setscope)
RT_IMPORT(pthread_mutexattr_init, mutexattr_init)
RT_IMPORT(pthread_mutexattr_destroy, mutexattr_destroy)
RT_IMPORT(pthread_mutexattr_settype, mutexattr_settype)
RT_IMPORT(pthread_key_create, key_create)
RT_IMPORT(pthread_key_delete, key_delete)
RT_IMPORT(pthread_setspecific, set_specific)
RT_IMPORT(pthread_getspecific, get_specific)
RT_IMPORT(pthread_once, p_once)
RT_IMPORT(pthread_mutex_init, mutex_init)
RT_IMPORT(pthread_mutex_destroy, mutex_destroy)
RT_IMPORT(pthread_mutex_lock, mutex_lock)
RT_IMPORT(pthread_mutex_trylock, mutex_trylock)
RT_IMPORT(pthread_mutex_unlock, mutex_unlock)
RT_IMPORT(pthread_cond_init, cond_init)
RT_IMPORT(pthread_cond_destroy, cond_destroy)
RT_IMPORT(pthread_cond_signal, cond_signal)
RT_IMPORT(pthread_cond_broadcast, cond_broadcast)
RT_IMPORT(pthread_cond_wait, cond_wait)
RT_IMPORT(pthread_cond_timedwait, cond_timedwait)
RT_IMPORT(sem_init, sem_init_)
RT_IMPORT(sem_destroy, sem_destroy_)
RT_IMPORT(sem_wait, sem_wait_)
RT_IMPORT(sem_trywait, sem_trywait_)
RT_IMPORT(sem_post, sem_post_)
RT_IMPORT(__errno, errno_location)

uint32_t guest_thread_entry() { return RT_TLS(t_cur) ? RT_TLS(t_cur)->start : 0; }
Cpu *main_cpu() { return g_main_cpu; }
void thread_counts(int &running, int &unjoined) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    running = unjoined = 0;
    for (const auto &[id, t] : g_threads) (t->finished ? unjoined : running)++;
}

// Hang reports (the watchdogs; and when the guest has many threads at once, a sign of threads
// that never end): each guest thread with its start routine, the guest mutex it is blocked on
// and that mutex's holder, and return addresses on its guest stack (read from another thread:
// a snapshot).
void report_threads(FILE *out) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    for (const auto &[id, t] : g_threads) {
        if (t->finished) continue;
        std::fprintf(out, "  guest thread %lu (start %08lx)%s", (unsigned long)t->host_id,
                     t->start ? (unsigned long)(t->start - g_load_base) : 0ul, t->host_id ? "" : " not started yet");
        if (const GuestThread *j = t->joining)
            std::fprintf(out, " joins thread %lu (pthread_t %08lx%s)", (unsigned long)j->host_id, (unsigned long)j->self,
                         g_threads.count(j->self) && g_threads.at(j->self) == j ? "" : ", no longer listed");
        const uint32_t m = t->waiting_for, v = m ? LD32(m) : 0;
        if (v & kHandleTag) {
            const SyncObj *o = installed(m, v);
            const GuestThread *holder = o && o->kind == SyncObj::Mutex ? o->owner : nullptr;
            std::fprintf(out, " waits for mutex %08lx, held by thread %lu", (unsigned long)m,
                         holder ? (unsigned long)holder->host_id : 0ul);
        }
        std::fprintf(out, ", callers:");
        if (const Cpu *cpu = t->cpu) {  // words just after a BL / BLX into the game: link addresses
            const uint32_t sp = cpu->r[13], top = guest_stack_top(cpu);
            int found = 0;
            for (uint32_t a = sp & ~3u; sp && a < top && a - sp < (256u << 10) && found < 12; a += 4) {
                if (!guest_mapped(a)) break;
                const uint32_t w = LD32(a) & ~1u;
                if (w < g_load_base + 4 || w >= g_load_base + (32u << 20) || !guest_mapped(w - 4)) continue;
                const uint32_t insn = LD32(w - 4);
                if ((insn & 0x0f000000u) != 0x0b000000u && (insn & 0xfe000000u) != 0xfa000000u && (insn & 0x0ffffff0u) != 0x012fff30u)
                    continue;
                std::fprintf(out, " %08lx", (unsigned long)(w - g_load_base));
                found++;
            }
        }
        std::fputc('\n', out);
    }
}

void log_game_thread_callers() {
    if (g_main_cpu) log_guest_callers(g_main_cpu);  // read from another thread: a snapshot, for hangs
}

}  // namespace rt

// The compiled code's direct calls (runtime/recomp.h, recomp/arm2c.py INLINE_IMPORTS): the
// game's most frequent thread calls without the import table. A lock that has to wait is timed
// as the import was ("waits" in the performance log).
extern "C" uint32_t rt_im_getspecific(uint32_t key) { return rt::get_specific(key); }
extern "C" uint32_t rt_im_self(void) { return rt::p_self(); }
extern "C" uint32_t rt_im_mutex_lock(uint32_t m) {
    rt::SyncObj *o = rt::lookup(m, rt::SyncObj::Mutex);
    if (!o->mutex.try_lock()) {
        const uint64_t start = rt::os::ticks();
        rt::GuestThread *t = RT_TLS(rt::t_cur);
        if (t) t->waiting_for = m;
        o->mutex.lock();
        if (t) t->waiting_for = 0;
        if (t && t->cpu == rt::g_main_cpu) rt::perf_add(rt::kPerfWait, int64_t(rt::os::ticks() - start));
    }
    rt::took(o);
    return 0;
}
extern "C" uint32_t rt_im_mutex_unlock(uint32_t m) { return uint32_t(rt::mutex_unlock(m)); }
