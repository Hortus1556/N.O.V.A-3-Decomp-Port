// Thread-local variables of the runtime and host: RT_THREAD_LOCAL(T, name) declares one,
// RT_TLS(name) is the calling thread's T. Everywhere but the GameCube, plain thread_local.
//
// The GameCube cannot use thread_local: GCC's thread pointer (r2) is the EABI small-data base
// that libogc2, newlib and libstdc++ are built to use, libogc2 sets up no TLS block, and this
// devkitPPC has no emulated TLS. There a variable is an array with one T per LWP thread slot (the
// index in the thread's handle), and a thread starting in a slot gets fresh values (os_gc.cpp).
#pragma once

#ifdef __GAMECUBE__
#include <cstdint>

namespace rt {
constexpr int kThreadSlots = 48;  // libogc2's LWP_MAX_THREADS (tools/gc/libogc2.patch)
int thread_slot();                // the calling thread's slot (os_gc.cpp)
// Each variable registers a function that resets one slot, called when a thread starts there.
void register_thread_slots(void (*reset)(void *self, int slot), void *self);

template <class T>
class ThreadSlots {
public:
    ThreadSlots() {
        register_thread_slots([](void *self, int slot) { static_cast<ThreadSlots *>(self)->values_[slot] = T{}; }, this);
    }
    T &get() { return values_[thread_slot()]; }

private:
    T values_[kThreadSlots]{};
};
}  // namespace rt

#define RT_THREAD_LOCAL(T, name) ::rt::ThreadSlots<T> name
#define RT_TLS(name) ((name).get())
#else
#define RT_THREAD_LOCAL(T, name) thread_local T name
#define RT_TLS(name) (name)
#endif
