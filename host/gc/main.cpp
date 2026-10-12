// N.O.V.A. 3 on the Nintendo GameCube (docs/gamecube.md). The same recompiled game, runtime and
// game-side host modules as the other ports; this file is the GameCube's counterpart of
// host/main.cpp:
//  * memory: MEM1 is split between the host's heap and the pager's frames; ARAM holds evicted
//    pages; the translated code (gcode.bin) is paged in from the game files through instruction
//    faults (runtime/gc_pager.c);
//  * the game files: an SD card's nova3/ folder when one is there (SD2SP2, or an SD adapter in
//    memory card slot A or B: "sd:", FAT through libdvm; the player's files and the log go there
//    too), else the disc ("dvd:", Dolphin or an optical drive emulator) with the player's files
//    in memory ("ram:");
//  * display: GX (runtime/gles_gx.cpp), its textures in a pool of MEM1; the log goes to the IPL
//    UART (Dolphin's OSReport log, runtime/gc_report.c);
//  * input: the controller in port 1 (host/input.cpp), sound: the audio interface (host/audio.cpp).
#include "audio.h"
#include "controls_menu.h"
#include "display.h"
#include "eventlog.h"
#include "gles_gx.h"
#include "hooks.h"
#include "input.h"
#include "jni.h"
#include "os.h"
#include "runtime.h"
#include "settings.h"

extern "C" {
#include "gc_dvdfs.h"
#include "gc_pager.h"
#include "gc_report.h"
}
#include "gc_ramfs.h"

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <sdcard/gcsd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>

extern "C" char __gcode_start[], __gcode_end[];  // tools/gc/ld/ogc.ld

namespace {

std::string g_data = "dvd:/nova3_data";  // the game files (read-only)
std::string g_root = "ram:/nova3";       // the player's files
std::string g_code = "dvd:/gcode.bin";   // the translated code

// An SD card with the game (nova3/gcode.bin): SD2SP2 (serial port 2), then the memory card
// slots' adapters. Its nova3/ folder then holds the game files, the player's files and the log.
// The probe runs on a thread of its own for at most 5 s: libogc2's SD driver can wait forever for
// a device that does not answer (Dolphin's AD16 on serial port 2), and the console then plays
// from the disc.
int g_sd_found = -1;  // the probe's result: the card's index, or -1
volatile bool g_sd_done;
void *sd_probe(void *) {
    DISC_INTERFACE *const cards[] = {&__io_gcsd2, &__io_gcsda, &__io_gcsdb};
    for (int i = 0; i < 3; i++) {
        if (!cards[i]->startup(cards[i]) || !cards[i]->isInserted(cards[i])) continue;
        if (!fatMountSimple("sd", cards[i])) {
            cards[i]->shutdown(cards[i]);
            continue;
        }
        struct stat st;
        if (stat("sd:/nova3/gcode.bin", &st) == 0) {
            g_sd_found = i;
            break;
        }
        fatUnmount("sd");
    }
    g_sd_done = true;
    return nullptr;
}
bool mount_sd() {
    static uint8_t stack[32 * 1024] __attribute__((aligned(32)));
    lwp_t thread;
    if (LWP_CreateThread(&thread, sd_probe, nullptr, stack, sizeof stack, LWP_PRIO_IDLE + 1) != 0) return false;
    const uint64_t t0 = gettime();
    while (!g_sd_done && ticks_to_millisecs(gettime() - t0) < 5000) usleep(10000);
    const char *const names[] = {"SD2SP2", "slot A", "slot B"};
    if (!g_sd_done) {
        std::printf("gc: the SD card probe did not finish (no card): the disc's game\n");
        return false;
    }
    if (g_sd_found < 0) return false;
    g_data = "sd:/nova3/nova3_data", g_root = "sd:/nova3/user", g_code = "sd:/nova3/gcode.bin";
    gc_report_open("sd:/nova3/log.txt");
    std::printf("gc: the game from the SD card in %s\n", names[g_sd_found]);
    return true;
}

// MEM1's arena: the host's heap keeps kHostReserve (the runtime's tables, the GL objects, thread
// stacks, the frame buffers and the GPU's FIFO), the textures kTexturePool; the pager takes the
// rest as frames. To be set from the measured peaks.
constexpr uint32_t kHostReserve = 6656u << 10;
constexpr uint32_t kTexturePool = 5632u << 10;
void *g_texture_pool;
uint32_t g_mem1_carved;  // the frames and the texture pool (allocated from the host's heap)

void init_memory() {
    const uint32_t aram_base = AR_Init(nullptr, 0);
    const uint32_t aram_size = AR_GetSize() - aram_base;
    const uint32_t arena = uint32_t(reinterpret_cast<uintptr_t>(SYS_GetArenaHi())) -
                           uint32_t(reinterpret_cast<uintptr_t>(SYS_GetArenaLo()));
    const uint32_t frames = (arena - kHostReserve - kTexturePool) & ~0xfffu;
    g_texture_pool = memalign(32, kTexturePool);
    void *mem = memalign(4096, frames);
    if (!mem || !g_texture_pool) {
        std::printf("gc: FAIL: no %u KB for the pager's frames\n", unsigned(frames >> 10));
        std::exit(1);
    }
    g_mem1_carved = frames + kTexturePool;
    pager_init(mem, frames, aram_base, aram_size);
    rt::log("memory: MEM1 arena %u KB: pager frames %u KB, textures %u KB, host %u KB; ARAM %u KB", unsigned(arena >> 10),
            unsigned(frames >> 10), unsigned(kTexturePool >> 10), unsigned(kHostReserve >> 10), unsigned(aram_size >> 10));
}

// The translated code (tools/gc/ld/ogc.ld puts it at __gcode_start) from gcode.bin.
bool map_code() {
    const uint32_t start = uint32_t(reinterpret_cast<uintptr_t>(__gcode_start));
    const uint32_t size = uint32_t(__gcode_end - __gcode_start);
    return rt::os::map_file(start, size, g_code.c_str(), 0, true);
}

// The host's heap at a point of the start (bring-up: what takes MEM1)
void heap_mark(const char *what) {
    rt::log("host heap after %s: %u KB", what, unsigned((uint32_t(mallinfo().uordblks) - g_mem1_carved) >> 10));
}

struct Boot {
    settings::Settings cfg;
    int width = 640, height = 480;
    std::atomic<bool> done{false};
};

void boot_and_play(Boot &boot) {
    const std::string pkg = "com/gameloft/android/ANMP/GloftNAHM/";
    const std::string jpkg = "Java_com_gameloft_android_ANMP_GloftNAHM_";
    auto native = [&](const std::string &cls, const std::string &method, std::initializer_list<uint32_t> args = {}) {
        std::string jcls = cls;
        for (char &ch : jcls) if (ch == '/') ch = '_';
        rt::log("-> %s.%s", cls.c_str(), method.c_str());
        return jni::call_native((jpkg + jcls + "_" + method).c_str(), pkg + cls, args);
    };
    jni::set_screen_size(boot.width, boot.height);
    jni::init();
    input::register_java();
    audio::register_java();
    if (uint32_t onload = rt::export_address("JNI_OnLoad")) {
        uint32_t version = rt::call(rt::current_cpu(), onload, {jni::vm(), 0});
        rt::log("JNI_OnLoad -> %08x", version);
        heap_mark("JNI_OnLoad");
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
    native("GL2JNILib", "resize", {uint32_t(boot.width), uint32_t(boot.height)});

    heap_mark("the game's init");
    const std::string lib = jpkg + "GL2JNILib_";
    bool in_game = false;
    for (int f = 0;; f++) {
        const uint64_t t0 = gettime();
        input::update();
        for (const input::Event &e : input::drain()) {
            if (e.kind == input::Event::Touch)
                jni::call_native((lib + "touchEvent").c_str(), pkg + "GL2JNILib",
                                 {uint32_t(e.a), uint32_t(e.b), uint32_t(e.c), uint32_t(e.d)});
            else if (e.kind != input::Event::Text)
                jni::call_native((lib + (e.a ? "OnKeyDown" : "OnKeyUp")).c_str(), pkg + "GL2JNILib", {uint32_t(e.b)});
        }
        input::set_gameplay(in_game);
        if (input::gameplay()) hooks::apply_controls(rt::current_cpu(), input::take_controls(), {});
        else hooks::apply_controls(rt::current_cpu(), {}, input::take_menu_nav());
        eventlog::frame(rt::current_cpu());
        hooks::auto_continue_loading(rt::current_cpu());
        hooks::watch_section_fall(rt::current_cpu());
        hooks::kill_fallen_enemies(rt::current_cpu());
        controls_menu::frame(rt::current_cpu());
        jni::call_native((lib + "step").c_str(), pkg + "GL2JNILib", {});
        gx::swap();
        in_game = input::consume_ingame_frame();
        rt::watchdog_frame();
        if (f < 10 || f % 300 == 0) rt::log("frame %d: %u ms", f, unsigned(ticks_to_millisecs(gettime() - t0)));
        if (f % 300 == 0) {
            const gx::Stats gs = gx::take_stats();
            rt::log("gx: %u frames: %u draws of %u, %u vertices; textures %u KB in MEM1, %u uploads, %u from ARAM, %u out",
                    gs.frames, gs.draws, gs.draws_requested, gs.vertices, unsigned(gs.tex_bytes >> 10), gs.tex_uploads,
                    gs.tex_fills, gs.tex_evictions);
        }
        // the host's heap (beside the frames and the texture pool) whenever it grew by 256 KB
        static uint32_t logged_heap;
        const uint32_t heap = uint32_t(mallinfo().uordblks) - g_mem1_carved;
        if (heap > logged_heap + (256u << 10)) {
            logged_heap = heap;
            const gx::Stats gs = gx::peek_stats();
            rt::log("frame %d: host heap %u KB, textures %u KB", f, unsigned(heap >> 10), unsigned(gs.tex_bytes >> 10));
        }
    }
}

void game_thread(void *param) {
    auto &boot = *static_cast<Boot *>(param);
    rt::init_memory();
    heap_mark("rt::init_memory");
    rt::install_crash_handler();
    rt::start_watchdog(15);
    rt::FsConfig fs;
    fs.data_dir = g_root + "/scratch";
    fs.save_dir = g_root + "/save";
    fs.obb_dir = g_data;
    rt::init_fs(fs);
    heap_mark("init_fs");
    static rt::LoadedLib lib = rt::load_library((g_data + "/libGloftNAHM.so").c_str());
    rt::set_main_library(&lib);
    rt::set_crash_library(&lib);
    Cpu *cpu = rt::init_main_thread();
    rt::log("loaded the library: %08x-%08x, %u static constructors", lib.base, lib.end, lib.init_array_count);
    for (uint32_t i = 0; i < lib.init_array_count; i++) {
        uint32_t fn = LD32(lib.init_array + 4 * i);
        if (fn == 0 || fn == 0xffffffffu) continue;
        rt::call(cpu, fn, {});
    }
    rt::log("static constructors done");
    heap_mark("the static constructors");
    if (!rt::install_gx_gl(g_texture_pool, kTexturePool)) rt::fatal("the GL functions could not be loaded");
    boot.width = gx::screen_width(), boot.height = gx::screen_height();
    input::set_screen(boot.width, boot.height);
    heap_mark("GX");
    boot_and_play(boot);
    boot.done = true;
}

}  // namespace

// The in-game Graphics page (host/display.h): nothing to change on the GameCube yet.
namespace display {
bool apply(const settings::Settings &) { return true; }
std::vector<std::pair<int, int>> resolutions() { return {{640, 480}}; }
void save(const settings::Settings &) { input::save_config(); }
}  // namespace display

int main(int, char **) {
    gc_report_install();
    VIDEO_Init();
    PAD_Init();
    std::printf("gc: N.O.V.A. 3 for the GameCube, built %s %s\n", __DATE__, __TIME__);
    init_memory();
    gc_ramfs_mount();
    if (!mount_sd() && !gc_dvdfs_mount()) {
        std::printf("gc: FAIL: no game: an SD card with nova3/gcode.bin, or the disc\n");
        return 1;
    }
    mkdir(g_root.c_str(), 0777);
    heap_mark("the file systems");
    if (!map_code()) {
        std::printf("gc: FAIL: no translated code (gcode.bin)\n");
        return 1;
    }
    static Boot boot;
    boot.cfg = settings::load(g_root + "/nova3.ini");
    input::configure(boot.cfg, g_root + "/nova3.ini");
    input::set_screen(boot.width, boot.height);
    audio::set_volume(boot.cfg.volume / 100.0f);
    if (!rt::os::start_thread(game_thread, &boot, 0)) {
        std::printf("gc: FAIL: no game thread\n");
        return 1;
    }
    while (!boot.done) usleep(100000);
    rt::os::exit_process(0);
}
