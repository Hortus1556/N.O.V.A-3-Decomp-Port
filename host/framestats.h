// Performance report for the log (both builds), meant for diagnosing real hardware from a log:
//  - every report window: frame rate and frame times (average, worst, late frames); the game
//    thread's step split into GL calls (the driver), file I/O, waits (locks, sleeps, socket
//    polls), memory allocation, and the rest (the game's code and the runtime); the swap
//    (presenting: waiting for the GPU / VSync); the game thread's CPU load (100% = its core is
//    saturated: CPU-bound) and the process's (Windows); then the GL calls per frame;
//  - every frame over 50 ms: a "slow frame" line with that frame's own split (at most 10 per
//    window), which tells a loading hitch (file I/O) from a driver stall or a burst of game work.
#pragma once
#include "jni.h"
#include "os.h"
#include "runtime.h"

#include <algorithm>
#include "platform.h"
#ifdef NOVA3_CONSOLE
#include <malloc.h>
#endif
#include <cstdio>
#include <cstdint>

struct FrameStats {
    int64_t last = 0, step_ns = 0, swap_ns = 0, total_ns = 0, worst_ns = 0;
    int frames = 0, over20 = 0, over34 = 0, slow_logged = 0;
    int64_t window_start = 0, cpu0 = -1, proc0 = -1;
    uint64_t ticks0 = 0;
    double ns_per_tick = 0;  // the tick rate, measured against monotonic_ns per window
#ifdef __3DS__
    rt::IoStats io_last{};   // the file requests at the last report
#endif

    int64_t mark() const { return rt::os::monotonic_ns(); }
    // at the end of each frame: when its step started, when its swap started, now
    void add(int64_t step_start, int64_t swap_start, int64_t end) {
        rt::PerfTimes f = rt::perf_frame_end();
        step_ns += swap_start - step_start;
        swap_ns += end - swap_start;
        if (!window_start) begin(end);
        if (last) {
            const int64_t frame = end - last;
            total_ns += frame;
            worst_ns = std::max(worst_ns, frame);
            over20 += frame > 20'000'000;
            over34 += frame > 34'000'000;
            frames++;
            if (frame > 50'000'000 && slow_logged < 10) {
                slow_logged++;
                const uint64_t t = rt::os::ticks();
                rt::perf_scale(f, ns_per_tick > 0 ? ns_per_tick : t > ticks0 ? double(end - window_start) / double(t - ticks0) : 1.0);
                int64_t known = 0;
                for (int64_t ns : f.ns) known += ns;
                const int64_t step = swap_start - step_start;
                char gl[24] = "-";  // (the 3DS times GL calls in one frame of 8: "game code" then includes them)
                if (f.gl_timed) std::snprintf(gl, sizeof gl, "%.1f ms", f.ns[rt::kPerfGl] / 1e6);
                rt::log("slow frame: %.1f ms (step %.1f, swap %.1f): GL %s, file I/O %.1f ms (%u calls), waits %.1f ms, "
                        "alloc %.1f ms, game code %.1f ms",
                        frame / 1e6, step / 1e6, (end - swap_start) / 1e6, gl, f.ns[rt::kPerfFile] / 1e6,
                        f.calls[rt::kPerfFile], f.ns[rt::kPerfWait] / 1e6, f.ns[rt::kPerfAlloc] / 1e6,
                        std::max<int64_t>(0, step - known) / 1e6);
            }
        }
        last = end;
    }
    void begin(int64_t now) {
        window_start = now;
        ticks0 = rt::os::ticks();
        cpu0 = rt::os::thread_cpu_ns();
        proc0 = rt::os::process_cpu_ns();
    }
    // from the game thread (its CPU time is the calling thread's)
    void log(int frame) {
        rt::PerfTimes p = rt::perf_take();
        const int64_t now = rt::os::monotonic_ns(), cpu = rt::os::thread_cpu_ns(), proc = rt::os::process_cpu_ns();
        if (const uint64_t t = rt::os::ticks(); window_start && t > ticks0) ns_per_tick = double(now - window_start) / double(t - ticks0);
        rt::perf_scale(p, ns_per_tick > 0 ? ns_per_tick : 1.0);
        if (frames && window_start) {
            const double n = frames, wall = double(now - window_start);
            int64_t known = 0;
            for (int64_t ns : p.ns) known += ns;
            char load[96] = "";
            if (cpu >= 0 && cpu0 >= 0) {
                const int len = std::snprintf(load, sizeof load, ", game thread CPU %.0f%%", 100.0 * double(cpu - cpu0) / wall);
#ifndef NOVA3_CONSOLE  // (the consoles; the Switch's process_cpu_ns is wall time: no per-process CPU time there)
                if (proc >= 0 && proc0 >= 0 && len > 0)
                    std::snprintf(load + len, sizeof load - size_t(len), ", process CPU %.0f%%", 100.0 * double(proc - proc0) / wall);
#endif
            }
            rt::log("perf: frame %d: %.1f fps, frame %.2f ms (worst %.1f), late %d >20 ms, %d >34 ms | step %.2f ms = GL %.2f + "
                    "file I/O %.2f + waits %.2f + alloc %.2f + game code %.2f | swap %.2f ms%s",
                    frame, 1e9 * n / double(total_ns), total_ns / 1e6 / n, worst_ns / 1e6, over20, over34, step_ns / 1e6 / n,
                    p.ns[rt::kPerfGl] / 1e6 / n, p.ns[rt::kPerfFile] / 1e6 / n, p.ns[rt::kPerfWait] / 1e6 / n,
                    p.ns[rt::kPerfAlloc] / 1e6 / n, std::max<int64_t>(0, step_ns - known) / 1e6 / n, swap_ns / 1e6 / n, load);
        }
        rt::gl_stats_log(frames);
        rt::import_profile_log(frames);
        int running = 0, unjoined = 0;
        rt::thread_counts(running, unjoined);
#ifdef NOVA3_CONSOLE
        rt::log("memory: host heap in use %d MB, %zu JNI objects, %d threads (%d ended, not joined)", int(mallinfo().uordblks >> 20),
                jni::live_objects(), running, unjoined);
#else
        rt::log("memory: %zu JNI objects, %d threads (%d ended, not joined)", jni::live_objects(), running, unjoined);
#endif
        {
            uint64_t live, peak, span, tex, tex3ds, targets, bufs;
            rt::heap_usage(live, peak, span);
            rt::gl_memory(tex, tex3ds, targets, bufs);
            rt::log("memory: guest heap %.1f MB live (peak %.1f, extent %.1f) | GL textures %.1f MB (3DS formats %.1f; render targets %.1f), "
                    "buffers %.1f MB", live / 1048576.0, peak / 1048576.0, span / 1048576.0, tex / 1048576.0, tex3ds / 1048576.0,
                    targets / 1048576.0, bufs / 1048576.0);
        }
#ifdef __3DS__
        {  // the packed archives' blocks unpacked in the window (runtime/libc_io.cpp), on any thread
            rt::IoStats io{};
            rt::io_stats(io);
            if (io.unpacked_bytes > io_last.unpacked_bytes)
                rt::log("files: %.2f MB unpacked in %.0f ms, %.2f MB read in %llu requests", (io.unpacked_bytes - io_last.unpacked_bytes) / 1048576.0,
                        double(io.unpack_ticks - io_last.unpack_ticks) * ns_per_tick / 1e6, (io.read_bytes - io_last.read_bytes) / 1048576.0,
                        (unsigned long long)(io.reads - io_last.reads));
            io_last = io;
        }
#endif
        step_ns = swap_ns = total_ns = worst_ns = 0;
        frames = over20 = over34 = slow_logged = 0;
        begin(now);
    }
};
