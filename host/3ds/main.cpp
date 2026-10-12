// N.O.V.A. 3 on the New Nintendo 3DS. The game's own ARM code runs natively
// (runtime/native_3ds.cpp: a so-loader with the ARMv6K patch set and the hook table), with the
// same runtime and game-side host modules as the PC and Switch builds (hooks, offline, prompts,
// Controls page, input bindings, Vox audio emulation); this file is the 3DS's counterpart of
// host/switch/main.cpp:
//  * the game files (the console set of tools/3ds_slimdata.py: the library's compressed copy and
//    the OBBs' contents) in the title's RomFS, romfs:/nova3_data/ (tools/3ds_cia.py), or, when
//    present, on the SD card: the RomFS image sdmc:/3ds/nova3/nova3_data.romfs (console testing:
//    tools/3ds_package.py --sd) or the folder sdmc:/3ds/nova3/nova3_data/ (Azahar: 3ds_run.py
//    --deploy); the patch set from the RomFS;
//  * the player's files: sdmc:/3ds/nova3/ holds save/ (save games), nova3.ini, the log nova3.log
//    and scratch/ (the guest's /sdcard, cache and /tmp);
//  * display: at 30 fps (60 selectable on the Graphics page), drawn by runtime/gles.cpp over the
//    PICA200 backend (runtime/gles_pica.cpp, citro3d): the menus on the bottom (touch) screen at
//    320x240, play on the top screen at 400x240 (Screens, below); the other screen shows the last
//    frame of play dimmed (top) or a status line (bottom). The test script's `headless` draws
//    nothing (gles_null.cpp);
//  * input: the buttons, circle pad, C-stick and touch screen through libctru (host/input.cpp);
//  * memory: the 178 MB of the New 3DS's extended mode (the .cia's exheader), laid out by
//    runtime/os_3ds.cpp.
#include "audio.h"
#include "controls_menu.h"
#include "display.h"
#include "eventlog.h"
#include "framestats.h"
#include "gles_pica.h"
#include "hooks.h"
#include "input.h"
#include "jni.h"
#include "os.h"
#include "runtime.h"
#include "settings.h"

#include <3ds.h>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <malloc.h>
#include <sstream>
#include <string>
#include <mutex>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <vector>

extern "C" u32 __ctru_heap_size, __ctru_linear_heap_size;
namespace rt {
void io_read_stats(uint64_t &bytes, uint64_t &requests);  // runtime/libc_io.cpp
void native_hook_profile_log(int frames);                  // runtime/native_3ds.cpp
namespace os {
bool app_suspended();    // runtime/os_3ds.cpp: in the HOME Menu or asleep
uint32_t app_resumes();  // returns from it so far
}  // namespace os
}

namespace {

const std::string kRoot = "sdmc:/3ds/nova3";
const std::string kSdData = kRoot + "/nova3_data", kRomfsData = "romfs:/nova3_data";
const std::string kSdImageData = "sdgame:/nova3_data";  // the SD card's image, mounted (mount_sd_image)
std::string g_data = kRomfsData;  // the game files (read-only): an SD card's copy when it is there
constexpr int kWidth = 400, kHeight = 240;          // the top screen (the bottom one: 320x240)
constexpr size_t kGameStack = size_t(4) << 20;      // the game thread's (Android's main thread had 8 MB)

struct Boot {
    settings::Settings cfg;
    std::atomic<bool> done{false};
    uint64_t thread = 0;  // the game thread (rt::os::thread_id): the one drawing
};
Boot *g_boot;
std::atomic<int64_t> g_frame_ns{33'333'333};  // the frame cap
bool g_headless = false;                       // test script: the null renderer
PrintConsole g_console;                        // the bottom screen's text, while play is on the top one
std::atomic<bool> g_game_on_bottom{false};     // the game draws on the bottom screen (no console there)

// What the in-game Graphics page can change (display::apply): the frame cap (30 or 60) and the
// field of view.
void apply_video(const settings::Settings &cfg) {
    const int fps = cfg.max_fps == 60 ? 60 : 30;
    g_frame_ns = 1'000'000'000 / fps;
    if (cfg.fov > 0) {
        const double h = cfg.fov * 3.14159265358979 / 180, v = 2 * std::atan(std::tan(h / 2) * 9.0 / 16.0);
        hooks::g_fov_scale = float(v / 0.60238);
    } else {
        hooks::g_fov_scale = 1.0f;
    }
    rt::log("video: %d fps cap, field of view %s", fps, cfg.fov > 0 ? std::to_string(cfg.fov).c_str() : "the game's");
}

// Unattended tests: sdmc:/3ds/nova3/test.txt, when present, scripts a run, one command per line
// (frames count from the first frame; inputs by their nova3.ini names), as on the Switch:
//   tap FRAME X Y            a touch at screen pixel X, Y
//   hold FRAME FRAMES INPUT  an input held for FRAMES frames
//   lua FRAME CODE           runs Lua in the game's script state (as --lua)
//   tour CHAPTER [TRIGGERS]  plays a campaign chapter through (eventlog::tour, as --tour)
//   bench CHAPTER FRAMES [walk]  measures FRAMES frames of a chapter's gameplay, then exits (as --bench)
//   speed FACTOR             game time per real time (as --speed)
//   quit FRAME               ends the run
//   trace_files              logs the game's file lookups
//   headless                 draws nothing (the null renderer)
//   env NAME VALUE           sets a debug environment variable
struct Script {
    struct Tap { int frame, x, y; };
    struct Hold { int frame, n; std::vector<int> codes; };
    struct Lua { int frame; std::string code; };
    std::vector<Tap> taps;
    std::vector<Hold> holds;
    std::vector<Lua> luas;
    int tour = -1, quit = -1;
    int bench = -1, bench_frames = 0;
    bool bench_walk = false;
    std::string tour_triggers;
    bool active = false;
};
Script g_script;

void load_script() {
    std::ifstream in(kRoot + "/test.txt");
    if (!in) return;
    g_script.active = true;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd;
        ls >> cmd;
        if (cmd == "tap") {
            Script::Tap t;
            ls >> t.frame >> t.x >> t.y;
            g_script.taps.push_back(t);
        } else if (cmd == "hold") {
            Script::Hold h;
            std::string name;
            ls >> h.frame >> h.n >> std::ws;
            std::getline(ls, name);
            h.codes = settings::parse_input(name);
            g_script.holds.push_back(h);
        } else if (cmd == "lua") {
            Script::Lua l;
            ls >> l.frame >> std::ws;
            std::getline(ls, l.code);
            g_script.luas.push_back(l);
        } else if (cmd == "tour") {
            ls >> g_script.tour >> g_script.tour_triggers;
        } else if (cmd == "bench") {  // bench CHAPTER FRAMES [walk]
            std::string walk;
            ls >> g_script.bench >> g_script.bench_frames >> walk;
            g_script.bench_walk = walk == "walk";
        } else if (cmd == "speed") {
            float v = 1;
            ls >> v;
            hooks::g_time_scale = std::clamp(v, 0.1f, 16.0f);
        } else if (cmd == "quit") {
            ls >> g_script.quit;
        } else if (cmd == "headless") {  // the null renderer: no GPU work at all
            g_headless = true;
        } else if (cmd == "trace_files") {
            setenv("NOVA3_TRACE_FILES", "1", 1);
        } else if (cmd == "env") {
            std::string name, value;
            ls >> name >> value;
            setenv(name.c_str(), value.c_str(), 1);
        }
    }
    rt::log("test script: %zu taps, %zu holds, %zu lua, tour %d, quit at %d", g_script.taps.size(), g_script.holds.size(),
            g_script.luas.size(), g_script.tour, g_script.quit);
}

void script_frame(int f) {
    for (const auto &t : g_script.taps) {
        if (t.frame == f) input::post({input::Event::Touch, 1, t.x, t.y, 0});
        if (t.frame + 3 == f) input::post({input::Event::Touch, 0, t.x, t.y, 0});
    }
    for (const auto &h : g_script.holds)
        for (int code : h.codes) {
            if (h.frame == f) input::script_input(code, true);
            if (h.frame + h.n == f) input::script_input(code, false);
        }
}

void script_after_step(int f) {
    for (const auto &l : g_script.luas)
        if (l.frame == f) eventlog::run_lua(rt::current_cpu(), l.code.c_str());
}

int64_t g_start_ns;  // main()'s start: the log's times (boot, loads) count from it
double since_start() { return (rt::os::monotonic_ns() - g_start_ns) / 1e9; }

// The bottom screen's console (while play is on the top screen) is written by the host alone,
// through console_printf. stdout, where the game's own printf goes ("WARNING: GAIA INIT ERROR":
// its online services, gone), is sent to the log, line by line: on the console it would have
// covered the screen, and in the menus written into the frame the game draws there.
const devoptab_t *g_console_dev;  // libctru's console device (consoleInit made it stdout's)
devoptab_t g_stdout_dev;
std::mutex g_stdout_lock;
std::string g_stdout_line;
void stdout_to_log() {
    g_console_dev = devoptab_list[STD_OUT];
    g_stdout_dev = *g_console_dev;
    g_stdout_dev.write_r = [](struct _reent *, void *, const char *ptr, size_t len) -> ssize_t {
        std::lock_guard<std::mutex> lock(g_stdout_lock);
        for (size_t i = 0; i < len; i++) {
            if (ptr[i] == '\n' || g_stdout_line.size() >= 512) {
                rt::log("stdout: %s", g_stdout_line.c_str());
                g_stdout_line.clear();
            }
            if (ptr[i] != '\n' && ptr[i] != '\r') g_stdout_line += ptr[i];
        }
        return ssize_t(len);
    };
    devoptab_list[STD_OUT] = &g_stdout_dev;
}
void console_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) g_console_dev->write_r(_REENT, nullptr, buf, std::min<size_t>(size_t(n), sizeof buf - 1));
}

// The status line on the bottom screen, in test runs (a test script) while play is on the top
// screen: what the game is doing. While the game loads (a frame over a second), the log gets the
// file system requests of each second: how fast the SD card delivers what the game asks for, and
// how much of the second went to it.
uint32_t g_linear_low = ~0u;  // the least free linear memory seen (GPU memory: textures, buffers)
void status(int frame, double fps) {
    g_linear_low = std::min<uint32_t>(g_linear_low, linearSpaceFree());
    static rt::IoStats io0;
    rt::IoStats io;
    rt::io_stats(io);
    if (fps < 2 && (io.reads > io0.reads || io.opens > io0.opens)) {
        const double ms = 1000.0 / SYSCLOCK_ARM11;
        rt::log("loading: frame %d at %.1f s: %.2f MB in %llu reads (%.0f ms), %llu opens (%.0f ms), %llu stats (%.0f ms), "
                "%.2f MB unpacked (%.0f ms) this second", frame, since_start(), (io.read_bytes - io0.read_bytes) / 1048576.0,
                (unsigned long long)(io.reads - io0.reads), (io.read_ticks - io0.read_ticks) * ms,
                (unsigned long long)(io.opens - io0.opens), (io.open_ticks - io0.open_ticks) * ms,
                (unsigned long long)(io.stats - io0.stats), (io.stat_ticks - io0.stat_ticks) * ms,
                (io.unpacked_bytes - io0.unpacked_bytes) / 1048576.0, (io.unpack_ticks - io0.unpack_ticks) * ms);
    }
    io0 = io;
    if (g_game_on_bottom || !g_script.active) return;
    uint64_t live, peak, span;
    rt::heap_usage(live, peak, span);
    console_printf("\x1b[1;1HN.O.V.A. 3%s\n\nframe %-8d %5.1f fps\ngame heap %5.1f MB (peak %5.1f)\nhost heap %5.1f MB\n"
                "linear free %5.1f MB\n", g_headless ? " (headless)" : "", frame, fps, live / 1048576.0, peak / 1048576.0,
                mallinfo().uordblks / 1048576.0, linearSpaceFree() / 1048576.0);
}

// One read of the RomFS at a time. libctru's RomFS device reads through a single FS file (the
// title's RomFS); a console's FS serves one session's requests one at a time anyway, but Azahar
// runs each read over 8 KB on a host thread of its own, and two at once (the game thread loading
// the title menu while the sound thread streamed its music) froze the emulation, which then
// crashed (a stack overflow) minutes later. The device's other calls make no requests (its
// tables are in memory).
std::mutex g_romfs_lock;
ssize_t (*g_romfs_read)(struct _reent *, void *, char *, size_t);  // libctru's (every RomFS mount's)
void serialize_romfs_reads(const char *device) {
    auto *dev = const_cast<devoptab_t *>(GetDeviceOpTab(device));
    if (!dev || !dev->read_r) return;
    if (!g_romfs_read) g_romfs_read = dev->read_r;
    if (dev->read_r != g_romfs_read) return;  // (already wrapped)
    dev->read_r = [](struct _reent *r, void *fd, char *ptr, size_t len) -> ssize_t {
        std::lock_guard<std::mutex> lock(g_romfs_lock);
        return g_romfs_read(r, fd, ptr, len);
    };
}

// Console testing (tools/3ds_package.py --sd): the game files as one RomFS image on the SD card,
// read through a single file system handle as the .cia's RomFS is. Not a folder of them: there
// every open file is a handle of the console's FS, and the game keeps its archives open; on a
// console further opens failed part way through the boot (the sound's DSP firmware among them).
bool mount_sd_image() {
    Handle h = 0;
    const std::string path = kRoot.substr(5) + "/nova3_data.romfs";  // (without "sdmc:")
    if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path.c_str()),
                                         FS_OPEN_READ, 0)))
        return false;
    if (const Result rc = romfsMountFromFile(h, 0, "sdgame"); R_FAILED(rc)) {
        rt::log("game files: sdmc:%s is not a RomFS image (%08lx)", path.c_str(), (unsigned long)rc);
        FSFILE_Close(h);
        return false;
    }
    serialize_romfs_reads("sdgame");
    return true;
}

// ---- Screens ------------------------------------------------------------------------------------
// The menus are drawn on the bottom screen at 320x240, where the touch screen points at them
// 1:1 (the game lays them out in its 1024x768 class there), and play on the top screen at
// 400x240 (docs/3ds.md, Platform). The game is told the size as Android tells it a surface size
// (GL2JNILib.resize, which the game takes at the start of its next step: its "Resizing window"
// log line; g_ScreenW / g_ScreenH at once), and the PICA backend links its screen target to that
// screen (no second target: no more VRAM). While the game is
// on the bottom screen, the top one shows the last frame of play dimmed (black before any), and
// a line of text during long steps; while it is on the top one, the bottom one is the console
// with the status line.

// The top screen's two framebuffers (BGR8, 240x400, column by column from the bottom left; they
// stay where they are), found at the start (find_top_buffers, game thread) and kept: libctru
// hands out only the one not shown, unless its double-buffering flag is cleared for the moment.
u8 *g_top_fb[2];
constexpr size_t kTopBytes = 400 * 240 * 3;
void find_top_buffers() {
    g_top_fb[0] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    gfxSetDoubleBuffering(GFX_TOP, false);
    g_top_fb[1] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    gfxSetDoubleBuffering(GFX_TOP, true);
}

// Both of the top screen's framebuffers at 3/8 of their brightness (the GPU's work done).
void dim_top() {
    for (u8 *b : g_top_fb) {
        auto *w = reinterpret_cast<uint32_t *>(b);
        for (size_t i = 0; i < kTopBytes / 4; i++) w[i] = ((w[i] >> 2) & 0x3f3f3f3fu) + ((w[i] >> 3) & 0x1f1f1f1fu);
        GSPGPU_FlushDataCache(b, kTopBytes);
    }
}

// A line of text on the top screen (both buffers) at character row `row` (of 30), in the
// console's 8x8 font, white on black, the rest of the row cleared.
void top_text(int row, const char *text) {
    const ConsoleFont &font = g_console.font;
    for (u8 *b : g_top_fb) {
        for (int x = 0; x < 400; x++) {
            const int ch = x / 8 < int(std::strlen(text)) ? (unsigned char)text[x / 8] : ' ';
            const int glyph = std::clamp(ch - int(font.asciiOffset), 0, int(font.numChars) - 1);
            for (int y = 0; y < 8; y++) {
                const bool on = (font.gfx[glyph * 8 + y] >> (7 - x % 8)) & 1;
                u8 *p = b + (size_t(x) * 240 + size_t(239 - (row * 8 + y))) * 3;
                p[0] = p[1] = p[2] = on ? 0xff : 0;
            }
        }
        GSPGPU_FlushDataCache(b, kTopBytes);
    }
}

// The bottom screen for the game (BGR8, double-buffered, cleared) or for the console (RGB565,
// single-buffered, cleared: as consoleInit leaves it). Between the GPU's frames.
void bottom_for_game() {
    g_game_on_bottom = true;
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    u8 *shown = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    for (u8 *b : {shown, gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr)}) {
        std::memset(b, 0, 320 * 240 * 3);
        GSPGPU_FlushDataCache(b, 320 * 240 * 3);
    }
}
void bottom_for_console() {
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    g_console.frameBuffer = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr));
    consoleSelect(&g_console);
    console_printf("\x1b[2J");
    if (!g_script.active) console_printf("\x1b[14;15HN.O.V.A. 3\x1b[16;10HSTART: pause menu");  // (no status line)
    gfxScreenSwapBuffers(GFX_BOTTOM, false);  // shows it in RGB565 (single-buffered: the same buffer)
    g_game_on_bottom = false;
}

bool g_on_bottom = true;  // the game's screen (the start: the boot's screens and the title menu)

// Console bring-up: what the GPU drew, as PNGs on the SD card (sdmc:/3ds/nova3/shots/fNNNNN.png,
// read back through the GPU: pica::read_screen): frames of the boot and the title menu, then one
// every 900 frames, 12 at most. Azahar's runs give the same frames to compare.
void screenshot_if_due(int f) {
    static int taken;
    const bool due = f == 60 || f == 150 || f == 300 || f == 450 || f == 600 || (f > 600 && f % 900 == 0);
    if (g_headless || taken >= 12 || !due) return;
    static const bool dir = mkdir((kRoot + "/shots").c_str(), 0777) == 0 || errno == EEXIST;
    char path[96];
    std::snprintf(path, sizeof path, "%s/shots/f%05d.png", kRoot.c_str(), f);
    const int w = g_on_bottom ? 320 : kWidth;
    if (dir && rt::gl_screenshot(path, w, kHeight)) {
        taken++;
        rt::log("screenshot: %s (%dx%d, the %s screen)", path, w, kHeight, g_on_bottom ? "bottom" : "top");
    }
}

// The game's screen size: told to the game, the touch screen and the backend.
void tell_screen_size(int w) {
    static const uint32_t screen_w = rt::export_address("g_ScreenW"), screen_h = rt::export_address("g_ScreenH");
    jni::set_screen_size(w, kHeight);
    input::set_screen(w, kHeight);
    if (screen_w && screen_h) ST32(screen_w, uint32_t(w)), ST32(screen_h, uint32_t(kHeight));
}
void set_game_screen(bool bottom, int frame) {
    if (bottom == g_on_bottom) return;
    g_on_bottom = bottom;
    rt::log("screen: %s at frame %d", bottom ? "menus on the bottom screen" : "play on the top screen", frame);
    if (!g_headless) {
        pica::set_screen(bottom);  // (waits for the GPU)
        if (bottom) {
            dim_top();
            bottom_for_game();
        } else {
            bottom_for_console();
        }
    }
    const int w = bottom ? 320 : kWidth;
    tell_screen_size(w);
    jni::call_native("Java_com_gameloft_android_ANMP_GloftNAHM_GL2JNILib_resize", "com/gameloft/android/ANMP/GloftNAHM/GL2JNILib",
                     {uint32_t(w), uint32_t(kHeight)});
}

// Where the game's current state belongs: play on the top screen (GS_InGame and the states shown
// over it while playing: death, respawn, scores, chat, the minigame), and the end of a level or
// match too: GS_EndLevel places its columns for the 1024x768 class at 320x240 and the labels
// ran into the values (the PC's 400x240 layout is right). Everything else goes on the bottom
// one (the title menu and its screens, loading screens, the pause menu and its pages). A level's
// loading screen goes up once the level is loaded (step 10: "touch the screen to continue"),
// before GS_InGame's first update makes the level's interface (CLevel::Init).
// `loading`: the GS_Loading on top, if any.
bool state_on_bottom(Cpu *cpu, uint32_t &loading) {
    static const uint32_t loading_vt = rt::export_address("_ZTV10GS_Loading") + 8;
    static const std::vector<uint32_t> play = [] {
        std::vector<uint32_t> v;
        for (const char *n : {"_ZTV9GS_InGame", "_ZTV13GS_PlayerDead", "_ZTV10GS_Respawn", "_ZTV16GS_IGMTimeFactor",
                              "_ZTV13GS_InGameChat", "_ZTV16GS_MPInGameScore", "_ZTV15GS_FaneMinigame", "_ZTV11GS_EndLevel",
                              "_ZTV17GS_EndMultiplayer"})
            if (const uint32_t a = rt::export_address(n)) v.push_back(a + 8);
        return v;
    }();
    loading = 0;
    const uint32_t top = hooks::top_state(cpu);
    if (!top) return true;
    const uint32_t vt = LD32(top);
    if (vt == loading_vt) {
        loading = top;
        return LD32(top + 0x444) < 10;
    }
    return std::find(play.begin(), play.end(), vt) == play.end();
}

}  // namespace
void dspfirm_ensure();  // host/3ds/dspfirm.cpp
extern "C" {  // runtime/guest_heap_3ds.c
int host_heap_walk(const char *where);
extern unsigned host_heap_calls;
unsigned host_heap_check_ticks(void);
}
namespace {

void boot_and_play(Boot &boot) {
    const std::string pkg = "com/gameloft/android/ANMP/GloftNAHM/";
    const std::string jpkg = "Java_com_gameloft_android_ANMP_GloftNAHM_";
    auto native = [&](const std::string &cls, const std::string &method, std::initializer_list<uint32_t> args = {}) {
        std::string jcls = cls;
        for (char &ch : jcls) if (ch == '/') ch = '_';
        rt::log("-> %s.%s", cls.c_str(), method.c_str());
        return jni::call_native((jpkg + jcls + "_" + method).c_str(), pkg + cls, args);
    };
    jni::set_screen_size(320, kHeight);  // (the boot's screens and the title menu: the bottom screen)
    jni::init();
    input::register_java();
    audio::register_java();
    if (uint32_t onload = rt::export_address("JNI_OnLoad")) {
        uint32_t version = rt::call(rt::current_cpu(), onload, {jni::vm(), 0});
        rt::log("JNI_OnLoad -> %08x", version);
        audio::select_backend();
    }
    native("GLUtils/SUtils", "nativeInit");
    native("GLUtils/Device", "nativeInit");
    native("SendInfo", "initMethods");
    native("DataSharing", "nativeInit");
    native("PushNotification/SimplifiedAndroidUtils", "nativeInit");
    auto social = [&](const std::string &cls) {
        std::string jcls = cls;
        for (char &ch : jcls) if (ch == '/') ch = '_';
        jni::call_native(("Java_com_gameloft_" + jcls + "_nativeInit").c_str(), "com/gameloft/" + cls, {});
    };
    social("GLSocialLib/PlatformAndroid");
    social("GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib");
    social("GLSocialLib/facebook/FacebookAndroidGLSocialLib");
    native("GL2JNILib", "init");
    native("GL2JNILib", "initGL");
    native("GL2JNILib", "InitViewSettings");
    native("GL2JNIActivity", "nativeInit");
    native("GL2JNILib", "resize", {320u, uint32_t(kHeight)});
    rt::log("boot: the game initialised at %.1f s", since_start());
    rt::log("host heap: the checks of a free and an allocation take %.2f us", host_heap_check_ticks() * 1e6 / SYSCLOCK_ARM11);

    const std::string lib = jpkg + "GL2JNILib_";
    bool in_game = false;
    FrameStats stats;
    int64_t next = rt::os::monotonic_ns(), second = next;
    int second_frames = 0, quick_frames = 0;
    double quick_since = 0;
    bool booted = false;
    std::vector<input::Event> deferred;  // Back released in play: given to the game a frame later
    for (int f = 0; aptMainLoop(); f++) {
        const int64_t step_start = stats.mark();
        if (f % 300 == 0 && f) {
            rt::log("time: frame %d at %.1f s", f, since_start());
            stats.log(f);
            rt::native_hook_profile_log(300);
            static unsigned calls0;
            rt::log("memory: linear free %.1f MB (lowest %.1f); host heap: %u allocations and frees", linearSpaceFree() / 1048576.0,
                    g_linear_low / 1048576.0, host_heap_calls - calls0);
            calls0 = host_heap_calls;
        }
        if (g_script.quit >= 0 && f >= g_script.quit) break;
        // the screen for the state on top; a level's loading screen that goes up holds the start
        // of play (its "touch the screen to continue") for a frame, so that the level's interface
        // is made at the new size
        uint32_t loading;
        const bool bottom = state_on_bottom(rt::current_cpu(), loading);
        hooks::g_font_width = loading ? 2 * kWidth : 0;  // the in-game fonts: those of the top screen
        bool hold_loading = false;
        if (bottom != g_on_bottom && deferred.empty()) {
            set_game_screen(bottom, f);
            if (loading && !bottom) ST8(loading + 0x44c, 0), hold_loading = true;
        }
        if (g_script.active) script_frame(f);
        input::update();
        std::vector<input::Event> events = std::move(deferred);
        deferred.clear();
        const size_t held_back = events.size();
        for (input::Event &e : input::drain()) events.push_back(std::move(e));
        for (size_t i = 0; i < events.size(); i++) {
            const input::Event &e = events[i];
            // Back released in play opens the pause menu, which is made at the screen's size when
            // it opens: the bottom screen's first, and Back a frame later
            if (i >= held_back && !g_on_bottom && in_game && e.kind == input::Event::Key && e.b == 4 && !e.a) {
                set_game_screen(true, f);
                deferred.push_back(e);
                continue;
            }
            if (e.kind == input::Event::Touch)
                jni::call_native((lib + "touchEvent").c_str(), pkg + "GL2JNILib",
                                 {uint32_t(e.a), uint32_t(e.b), uint32_t(e.c), uint32_t(e.d)});
            else if (e.kind == input::Event::Text)
                jni::call_native((jpkg + "GLUtils_SUtils_nativeSendKeyboardData").c_str(), pkg + "GLUtils/SUtils",
                                 {jni::new_string(e.text)});
            else
                jni::call_native((lib + (e.a ? "OnKeyDown" : "OnKeyUp")).c_str(), pkg + "GL2JNILib", {uint32_t(e.b)});
        }
        input::set_gameplay(in_game);
        if (input::gameplay()) hooks::apply_controls(rt::current_cpu(), input::take_controls(), {});
        else hooks::apply_controls(rt::current_cpu(), {}, input::take_menu_nav());
        eventlog::frame(rt::current_cpu());
        if (!hold_loading) hooks::auto_continue_loading(rt::current_cpu());
        hooks::watch_section_fall(rt::current_cpu());
        hooks::kill_fallen_enemies(rt::current_cpu());
        controls_menu::frame(rt::current_cpu());
        if (g_script.bench >= 0)
            eventlog::bench(rt::current_cpu(), g_script.bench, g_script.bench_frames, false, g_script.bench_walk);
        if (g_script.tour >= 0) eventlog::tour(rt::current_cpu(), g_script.tour, g_script.tour_triggers, "", kWidth, kHeight);
        jni::call_native((lib + "step").c_str(), pkg + "GL2JNILib", {});
        in_game = input::consume_ingame_frame();
        rt::watchdog_frame();
        if (g_script.active) script_after_step(f);
        // the frame cap (the renderer's VSync will pace it once it draws)
        const int64_t swap_start = stats.mark();
        next += g_frame_ns;
        const int64_t now = rt::os::monotonic_ns();
        if (next > now) rt::os::sleep_ns(next - now);
        else if (now - next > 100'000'000) next = now;  // far behind (loading): do not catch up
        second_frames++;
        if (const int64_t t = rt::os::monotonic_ns(); t - second >= 1'000'000'000) {
            status(f, second_frames * 1e9 / double(t - second));
            second = t;
            second_frames = 0;
        }
        if (!g_headless) pica::swap();  // (the bottom screen's console is single-buffered)
        screenshot_if_due(f);
        stats.add(step_start, swap_start, stats.mark());
        // NOVA3_HEAP_CHECK=1: newlib's heap walked after each frame of the boot and of loads
        static const bool heap_check = [] { const char *v = std::getenv("NOVA3_HEAP_CHECK"); return v && *v == '1'; }();
        if (heap_check && (!booted || swap_start - step_start > 100'000'000)) {
            char where[32];
            std::snprintf(where, sizeof where, "frame %d", f);
            host_heap_walk(where);
        }
        // the boot's end: the title menu running, 60 frames in a row with steps under 50 ms
        if (!booted) {
            quick_frames = swap_start - step_start < 50'000'000 ? quick_frames + 1 : 0;
            if (quick_frames == 1) quick_since = since_start();
            if (quick_frames == 60) {
                rt::log("boot: frames steady from frame %d, %.1f s after the start", f - 59, quick_since);
                rt::io_boot_done();
                booted = true;
            }
        }
    }
    rt::log("leaving (%s)", g_script.quit >= 0 ? "test script" : "HOME / close");
}

void game_thread(void *param) {
    auto &boot = *static_cast<Boot *>(param);
    boot.thread = rt::os::thread_id();
    rt::init_memory();
    rt::install_crash_handler();
    rt::start_watchdog(15);
    rt::FsConfig fs;
    fs.data_dir = kRoot + "/scratch";
    fs.save_dir = kRoot + "/save";
    fs.obb_dir = g_data;
    rt::init_fs(fs);
    rt::log("boot: the file system set up at %.1f s", since_start());
    static rt::LoadedLib lib = rt::load_library((g_data + "/libGloftNAHM.so").c_str());
    rt::set_main_library(&lib);
    rt::set_crash_library(&lib);
    Cpu *cpu = rt::init_main_thread();
    if (const char *v = std::getenv("NOVA3_LOAD_PROFILE"); v && *v == '1') rt::native_start_load_profile();
    rt::log("loaded the library at %.1f s: %08x-%08x, %u static constructors", since_start(), lib.base, lib.end,
            lib.init_array_count);
    for (uint32_t i = 0; i < lib.init_array_count; i++) {
        uint32_t fn = LD32(lib.init_array + 4 * i);
        if (fn == 0 || fn == 0xffffffffu) continue;
        rt::log("static constructor %u: link %08x", i, fn - lib.base);  // (a console start that stops here names it)
        rt::call(cpu, fn, {});
    }
    rt::log("static constructors done at %.1f s", since_start());
    apply_video(boot.cfg);
    if (g_headless) rt::install_null_gl();
    else if (!rt::install_pica_gl()) rt::fatal("the GPU (citro3d) could not start");
    if (!g_headless) {  // the game starts on the bottom screen, the top one black
        pica::set_screen(true);
        find_top_buffers();
        for (u8 *b : g_top_fb) std::memset(b, 0, kTopBytes), GSPGPU_FlushDataCache(b, kTopBytes);
        bottom_for_game();
    }
    // the game's ETC1 images over 1024 texels, made to fit the PICA offline (tools/3ds_glreplay/picapack.py)
    if (!g_headless) pica::set_texture_pack((g_data + "/pica_textures.pak").c_str());
    boot_and_play(boot);
    boot.done = true;
}

}  // namespace

// The in-game Graphics page (host/display.h): the frame cap and the field of view.
namespace display {
bool apply(const settings::Settings &cfg) {
    if (g_boot) g_boot->cfg = cfg;
    apply_video(cfg);
    return true;
}
std::vector<std::pair<int, int>> resolutions() { return {{kWidth, kHeight}}; }
void save(const settings::Settings &) { input::save_config(); }  // the live settings, rebinds included
}  // namespace display

// The process's end (rt::os::exit_process, through libctru's __appExit, before fs, hid and APT):
// the sound stopped (its channels would play on), the GPU's work finished and citro3d closed
// when no other thread may be drawing, and the screens given back to the system.
extern "C" void userAppExit() {
    audio::stop_output();
    if (!g_headless && g_boot && (g_boot->done || g_boot->thread == rt::os::thread_id())) pica::finish();
    gfxExit();
}

int main(int, char **) {
    g_start_ns = rt::os::monotonic_ns();
    osSetSpeedupEnable(true);       // 804 MHz and the L2 cache (the exheader allows them)
    APT_SetAppCpuTimeLimit(30);     // the system core: the log writer and the watchdog
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, &g_console);
    stdout_to_log();
    mkdir("sdmc:/3ds", 0777);  // a fresh SD card (or an emulator's) may not have them
    mkdir(kRoot.c_str(), 0777);
    std::freopen((kRoot + "/nova3.log").c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    rt::log("N.O.V.A. 3 for the New 3DS, built %s %s; clock %lld s since 1970", __DATE__, __TIME__,
            (long long)(rt::os::realtime_ns() / 1000000000));
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    rt::log("memory: application %lu KB (%lu KB free), heap %lu KB (game image %lu KB, newlib %lu KB), linear %lu KB%s%s",
            (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10), (unsigned long)(__ctru_heap_size >> 10),
            (unsigned long)(rt::kNativeImageReserve >> 10), (unsigned long)((__ctru_heap_size - rt::kNativeImageReserve) >> 10),
            (unsigned long)(__ctru_linear_heap_size >> 10), n3ds ? "" : " (not a New 3DS!)",
            rt::native_emulator() ? ", emulator" : "");
    if (R_FAILED(romfsInit())) rt::fatal("no RomFS (romfs:/v6patch.bin): install the .cia built by tools/3ds_cia.py");
    serialize_romfs_reads("romfs");
    load_script();
    dspfirm_ensure();  // (before the sound starts: extracted from the HOME Menu if missing)
    // sockets (soc:U): local Wi-Fi multiplayer and the game's own networking, which then fails
    // the way it does on the PC (no DNS, no route out) instead of finding no socket service
    static u32 *soc_buffer = static_cast<u32 *>(memalign(0x1000, 0x100000));
    if (const Result rc = soc_buffer ? socInit(soc_buffer, 0x100000) : -1; R_FAILED(rc))
        rt::log("network: soc:U unavailable (%08lx): no sockets", (unsigned long)rc);
    // a fatal error shows its message on the bottom screen for a few seconds, then the game closes
    rt::set_fatal_notify([](const char *msg) {
        audio::stop_output();
        if (g_game_on_bottom) bottom_for_console();  // (the GPU's last frame may still be in flight)
        console_printf("\x1b[2J\x1b[1;1HN.O.V.A. 3 stopped:\n\n%s\n\nDetails: sdmc:/3ds/nova3/nova3.log\n", msg);
        gfxFlushBuffers();
        gfxSwapBuffers();
        svcSleepThread(5'000'000'000LL);
    });
    // the library itself, or only its compressed copy (the console set); the SD card's copy first
    const auto has_game = [](const std::string &dir) {
        struct stat st;
        return stat((dir + "/libGloftNAHM.so").c_str(), &st) == 0 || stat((dir + "/libGloftNAHM.so.z").c_str(), &st) == 0;
    };
    if (mount_sd_image() && has_game(kSdImageData)) g_data = kSdImageData;
    else if (has_game(kSdData)) g_data = kSdData;
    else if (!has_game(kRomfsData))
        rt::fatal("the game files are missing: neither %s nor %s.romfs nor %s (a .cia built without them?)", kRomfsData.c_str(),
                  kSdData.c_str(), kSdData.c_str());
    rt::log("game files: %s%s", g_data.c_str(),
            g_data == kSdImageData ? " (the SD card's image nova3_data.romfs, ahead of the RomFS's)"
            : g_data == kSdData    ? " (the SD card's folder, ahead of the RomFS's: on a console its open files can run out)"
                                   : "");

    static Boot boot;
    struct stat ini;
    const bool first_start = stat((kRoot + "/nova3.ini").c_str(), &ini) != 0;
    boot.cfg = settings::load(kRoot + "/nova3.ini");
    if (first_start) settings::save(kRoot + "/nova3.ini", boot.cfg);
    g_boot = &boot;
    input::configure(boot.cfg, kRoot + "/nova3.ini");
    rt::net_set_peers(boot.cfg.peers);
    input::set_screen(320, kHeight);
    audio::set_volume(boot.cfg.volume / 100.0f);

    if (!rt::os::start_thread(game_thread, &boot, kGameStack)) rt::fatal("cannot start the game thread");
    // The game thread does everything (input, HOME / sleep through aptMainLoop); this one waits,
    // and shows on the bottom screen that a long step (a level load: over a minute in one call
    // of the game's) is going on: the top screen keeps the last frame meanwhile. It writes only
    // while no frame completes, when the game thread's status line does not.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2c);  // above the game thread (same core): it only wakes to look
    uint64_t last_frames = 0;
    int64_t still_since = rt::os::monotonic_ns();
    uint64_t bytes0 = 0, unused;
    uint32_t resumes = 0;
    while (!boot.done) {
        svcSleepThread(250'000'000);
        const uint64_t frames = rt::watchdog_frames();
        const int64_t now = rt::os::monotonic_ns();
        if (rt::os::app_suspended() || rt::os::app_resumes() != resumes) {  // the HOME Menu: no frames, no stall
            resumes = rt::os::app_resumes();
            last_frames = frames, still_since = now;
            continue;
        }
        if (frames != last_frames) {
            if (now - still_since > 1'250'000'000) {  // (the line below goes)
                if (g_game_on_bottom && g_top_fb[0]) top_text(28, "");
                else console_printf("\x1b[8;1H\x1b[K");
            }
            last_frames = frames, still_since = now;
            rt::io_read_stats(bytes0, unused);
            continue;
        }
        const int64_t still = now - still_since;
        // in a long stall, every 15 s: this thread runs (core 0, above the game thread, below
        // the sound feeder), and the feeder's wakes since (about 200 a second when it only
        // waits on the DSP); with the watchdog's report, which of them keeps the game thread out
        static int64_t alive_logged;
        if (still >= 15'000'000'000 && now - alive_logged >= 15'000'000'000) {
            static uint32_t wakes0;
            const uint32_t wakes = audio::feeder_wakes();
            rt::log("main thread: running, no frame for %lld s; sound feeder woke %lu times since", (long long)(still / 1'000'000'000),
                    (unsigned long)(wakes - wakes0));
            wakes0 = wakes;
            alive_logged = now;
        }
        if (frames == 0 || still < 1'000'000'000 || g_headless) continue;
        uint64_t bytes;
        rt::io_read_stats(bytes, unused);
        static const char spin[] = "|/-\\";
        char line[64];
        std::snprintf(line, sizeof line, "loading %c %3d s, %5.1f MB read", spin[(still / 250'000'000) % 4], int(still / 1'000'000'000),
                      (bytes - bytes0) / 1048576.0);
        if (g_game_on_bottom && g_top_fb[0]) top_text(28, line);  // (the bottom screen holds the game's loading screen)
        else console_printf("\x1b[8;1H%s\x1b[K", line);
    }
    rt::os::exit_process(0);  // (userAppExit: the sound stopped, the screens given back)
}
