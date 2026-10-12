// N.O.V.A. 3 on the Nintendo Switch (homebrew). The same recompiled game, runtime and
// game-side host modules as the PC build (hooks, offline, prompts, Controls page, input
// bindings, Vox audio emulation); this file is the Switch's counterpart of host/main.cpp:
//  * the game files (libGloftNAHM.so and the OBBs' contents, as tools/package.py lays them
//    out): the title's RomFS in the .nsp (tools/switch_nsp.py), else, for the .nro, the SD
//    card's sdmc:/switch/nova3/nova3_data/;
//  * the player's files on the SD card: sdmc:/switch/nova3/ holds save/ (save games),
//    nova3.ini, the log nova3.log and scratch/ (the guest's /sdcard, cache and /tmp);
//  * display: SDL2 (devkitPro) with GLES 2 on Mesa, 1920x1080 docked or 1280x720 handheld
//    (chosen at start);
//  * input: controllers and the touchscreen through libnx (host/input.cpp);
//  * memory: the game needs about 1.2 GB, so homebrew must run with an application's memory
//    (started by holding R while launching a game, or installed as its own title).
#include "audio.h"
#include "controls_menu.h"
#include "eventlog.h"
#include "framestats.h"
#include "hooks.h"
#include "input.h"
#include "jni.h"
#include "os.h"
#include "runtime.h"
#include "display.h"
#include "settings.h"

#include <SDL2/SDL.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <malloc.h>
#include <sys/stat.h>
#include <vector>

namespace {

const std::string kRoot = "sdmc:/switch/nova3";
std::string g_data = kRoot + "/nova3_data";  // the game files (read-only)

struct Boot {
    settings::Settings cfg;
    int width = 1280, height = 720;
    std::atomic<bool> done{false};
};

SDL_Window *g_window;
SDL_GLContext g_gl;
Boot *g_boot;

// Docked / handheld: the framebuffer is always 1920x1080; the game renders at the mode's size
// (1920x1080 docked, 1280x720 handheld) into its lower-left corner, and the display shows just
// that region (the window's crop), scaled to the screen. A mode change while playing only
// tells the game the new size (GL2JNILib.resize, as a phone rotating), no new surface.
bool g_docked = true;
int g_force_mode = -1;  // test scripts: 1 docked, 0 handheld

void set_render_size(Boot &boot, bool docked) {
    g_docked = docked;
    boot.width = docked ? 1920 : 1280;
    boot.height = docked ? 1080 : 720;
    // GL draws bottom-up: the lower-left corner is the last rows of the buffer
    nwindowSetCrop(nwindowGetDefault(), 0, 1080 - boot.height, boot.width, 1080);
    jni::set_screen_size(boot.width, boot.height);
    input::set_screen(boot.width, boot.height);
    rt::log("video: %dx%d (%s)", boot.width, boot.height, docked ? "docked" : "handheld");
}

// What the in-game Graphics page can change (display::apply): the swap interval from the frame
// cap (30 or 60) and VSync (on, or adaptive: a late frame shows at once instead of waiting a
// whole refresh), the field of view, anisotropic filtering. On the game thread (the GL context).
void apply_video(const settings::Settings &cfg) {
    const int interval = cfg.max_fps > 0 && cfg.max_fps <= 30 ? 2 : 1;
    const bool adaptive = cfg.vsync == settings::Settings::VSync::Adaptive && SDL_GL_SetSwapInterval(-interval) == 0;
    if (!adaptive) SDL_GL_SetSwapInterval(interval);
    rt::gl_set_anisotropy(float(cfg.anisotropy));
    if (cfg.fov > 0) {
        const double h = cfg.fov * 3.14159265358979 / 180, v = 2 * std::atan(std::tan(h / 2) * 9.0 / 16.0);
        hooks::g_fov_scale = float(v / 0.60238);
    } else {
        hooks::g_fov_scale = 1.0f;
    }
    rt::log("video: %d fps cap, vsync %s, field of view %s, anisotropy %dx", 60 / interval, adaptive ? "adaptive" : "on",
            cfg.fov > 0 ? std::to_string(cfg.fov).c_str() : "the game's", cfg.anisotropy);
}

// Unattended tests: sdmc:/switch/nova3/test.txt, when present, scripts a run, one command per
// line (frames count from the first frame; inputs by their nova3.ini names, e.g. "Pad A"):
//   shot FRAME FILE          a PNG of the screen into sdmc:/switch/nova3/FILE
//   tap FRAME X Y            a touch at screen pixel X, Y
//   hold FRAME FRAMES INPUT  an input held for FRAMES frames
//   lua FRAME CODE           runs Lua in the game's script state (as --lua)
//   tour CHAPTER [TRIGGERS]  plays a campaign chapter through (eventlog::tour, as --tour)
//   tour_shots               the tour's screenshot per checkpoint, into sdmc:/switch/nova3/tour
//   tour_stop SPAWN          the tour stops at that checkpoint (as --tour-stop): the other commands'
//                            frames then count from the stop
//   bench CHAPTER FRAMES [walk]  measures FRAMES frames of a chapter's gameplay, then exits (as --bench)
//   speed FACTOR             game time per real time (as --speed)
//   quit FRAME               ends the run
//   log_player FRAME         logs the player's state, position and object in reach (as --log-player)
//   trace_files              logs the game's file lookups
//   mode docked|handheld     acts as if the console were docked / in handheld mode
//   env NAME VALUE           sets a debug environment variable
struct Script {
    struct Shot { int frame; std::string file; };
    struct Tap { int frame, x, y; };
    struct Hold { int frame, n; std::vector<int> codes; };
    struct Lua { int frame; std::string code; };
    std::vector<Shot> shots;
    std::vector<Tap> taps;
    std::vector<Hold> holds;
    std::vector<Lua> luas;
    std::vector<int> log_player;
    int tour = -1, quit = -1, tour_stop = -1;
    int bench = -1, bench_frames = 0;
    bool bench_walk = false;
    std::string tour_triggers, tour_shots;
    bool active = false;
};
Script g_script;

extern "C" bool nova3_alloc_log;  // runtime/os_switch.cpp: logs large heap blocks

void load_script() {
    std::ifstream in(kRoot + "/test.txt");
    if (!in) return;
    g_script.active = true;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // (a script written on Windows)
        std::istringstream ls(line);
        std::string cmd;
        ls >> cmd;
        if (cmd == "shot") {
            Script::Shot s;
            ls >> s.frame >> s.file;
            g_script.shots.push_back(s);
        } else if (cmd == "tap") {
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
        } else if (cmd == "tour_stop") {
            ls >> g_script.tour_stop;
        } else if (cmd == "tour_shots") {
            g_script.tour_shots = kRoot + "/tour";
            mkdir(g_script.tour_shots.c_str(), 0777);
        } else if (cmd == "speed") {
            float v = 1;
            ls >> v;
            hooks::g_time_scale = std::clamp(v, 0.1f, 16.0f);
        } else if (cmd == "log_player") {
            int frame = 0;
            ls >> frame;
            g_script.log_player.push_back(frame);
        } else if (cmd == "quit") {
            ls >> g_script.quit;
        } else if (cmd == "mode") {  // mode docked|handheld: as if the console were (un)docked
            std::string m;
            ls >> m;
            g_force_mode = m == "docked" ? 1 : 0;
        } else if (cmd == "trace_files") {
            setenv("NOVA3_TRACE_FILES", "1", 1);
        } else if (cmd == "env") {  // env NAME VALUE: a debug variable (NOVA3_TOUR_SETTLE_MS, ...)
            std::string name, value;
            ls >> name >> value;
            setenv(name.c_str(), value.c_str(), 1);
        }
    }
    rt::log("test script: %zu shots, %zu taps, %zu holds, tour %d, quit at %d", g_script.shots.size(),
            g_script.taps.size(), g_script.holds.size(), g_script.tour, g_script.quit);
}

void script_frame(int frame) {
    const int f = eventlog::script_frame(frame, g_script.tour_stop >= 0);
    if (f < 0) return;
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

void script_after_step(int frame, int width, int height) {
    const int f = eventlog::script_frame(frame, g_script.tour_stop >= 0);
    if (f < 0) return;
    for (const auto &l : g_script.luas)
        if (l.frame == f) eventlog::run_lua(rt::current_cpu(), l.code.c_str());
    for (int frame : g_script.log_player)
        if (frame == f) eventlog::log_player(rt::current_cpu());
    for (const auto &s : g_script.shots)
        if (s.frame == f && rt::gl_screenshot((kRoot + "/" + s.file).c_str(), width, height))
            rt::log("screenshot: %s", s.file.c_str());
}

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

    const std::string lib = jpkg + "GL2JNILib_";
    bool in_game = false;
    FrameStats stats;
    for (int f = 0; appletMainLoop(); f++) {
        const int64_t step_start = stats.mark();
        if (f % 600 == 0 && f) stats.log(f);
        if (g_script.quit >= 0 && f >= g_script.quit) break;
        if (f % 30 == 0) {  // docked <-> handheld
            const bool docked = g_force_mode >= 0 ? g_force_mode == 1 : appletGetOperationMode() == AppletOperationMode_Console;
            if (docked != g_docked) {
                set_render_size(boot, docked);
                jni::call_native((lib + "resize").c_str(), pkg + "GL2JNILib", {uint32_t(boot.width), uint32_t(boot.height)});
            }
        }
        if (g_script.active) script_frame(f);
        input::update();
        for (const input::Event &e : input::drain()) {
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
        hooks::auto_continue_loading(rt::current_cpu());
        // loading screens run the CPU in boost mode (1785 MHz instead of 1020; the GPU at its
        // minimum, which a loading screen does not need), as retail games do
        // in the menus, once the player has left them alone for 2 s, the programs of earlier runs
        // are compiled ahead, one a frame (gles.cpp): a compile slows that frame, which would
        // make the menus feel sluggish while they are used
        if (!in_game && !input::gameplay() && input::idle_ns() > 2'000'000'000 && !hooks::loading(rt::current_cpu()))
            rt::gl_prewarm_step();
        static bool boosted = false, boost_failed = false;
        if (f % 10 == 0 && !boost_failed) {
            if (const bool load = hooks::loading(rt::current_cpu()); load != boosted) {
                const Result rc = appletSetCpuBoostMode(load ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
                boost_failed = R_FAILED(rc);
                boosted = load && !boost_failed;
                if (boost_failed) rt::log("cpu: boost mode not available (%x)", rc);
                else rt::log("cpu: boost %s", load ? "on (loading)" : "off");
            }
        }
        hooks::watch_section_fall(rt::current_cpu());
        hooks::kill_fallen_enemies(rt::current_cpu());
        controls_menu::frame(rt::current_cpu());
        if (g_script.bench >= 0)
            eventlog::bench(rt::current_cpu(), g_script.bench, g_script.bench_frames, false, g_script.bench_walk);
        if (g_script.tour >= 0)
            eventlog::tour(rt::current_cpu(), g_script.tour, g_script.tour_triggers, g_script.tour_shots, boot.width, boot.height,
                           g_script.tour_stop);
        jni::call_native((lib + "step").c_str(), pkg + "GL2JNILib", {});
        in_game = input::consume_ingame_frame();
        rt::watchdog_frame();
        if (g_script.active) script_after_step(f, boot.width, boot.height);
        const int64_t swap_start = stats.mark();
        SDL_GL_SwapWindow(g_window);
        stats.add(step_start, swap_start, stats.mark());
    }
    rt::log("leaving (%s)", g_script.quit >= 0 ? "test script" : "HOME / close");
}

void game_thread(void *param) {
    auto &boot = *static_cast<Boot *>(param);
    rt::init_memory();
    rt::install_crash_handler();
    rt::start_watchdog(15);
    rt::FsConfig fs;
    fs.data_dir = kRoot + "/scratch";
    fs.save_dir = kRoot + "/save";
    fs.obb_dir = g_data;
    rt::init_fs(fs);
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
    if (SDL_GL_MakeCurrent(g_window, g_gl) != 0) rt::fatal("SDL_GL_MakeCurrent: %s", SDL_GetError());
    apply_video(boot.cfg);
    rt::gl_set_shader_cache(kRoot + "/shadercache");  // compiled shaders: later loads skip Mesa's compiler
    if (!rt::gl_load([](const char *name) { return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name)); }))
        rt::fatal("missing GLES2 functions");
    boot_and_play(boot);
    boot.done = true;
}

}  // namespace

// The in-game Graphics page (host/display.h): what the Switch can change while playing.
namespace display {
bool apply(const settings::Settings &cfg) {
    if (g_boot) g_boot->cfg = cfg;
    apply_video(cfg);
    return true;
}
std::vector<std::pair<int, int>> resolutions() { return {{1920, 1080}, {1280, 720}}; }  // docked / handheld: automatic
void save(const settings::Settings &) { input::save_config(); }  // the live settings, rebinds included
}  // namespace display

int main(int, char **) {
    mkdir("sdmc:/switch", 0777);  // a fresh SD card (or an emulator's) may not have it
    mkdir(kRoot.c_str(), 0777);
    std::freopen((kRoot + "/nova3.log").c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    rt::log("N.O.V.A. 3 for the Switch, built %s %s", __DATE__, __TIME__);
    {
        extern char *fake_heap_start, *fake_heap_end;
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        rt::log("memory: application %llu MB, in use %llu MB, heap %llu MB", (unsigned long long)(total >> 20),
                (unsigned long long)(used >> 20), (unsigned long long)((fake_heap_end - fake_heap_start) >> 20));
    }
    load_script();
    nova3_alloc_log = std::getenv("NOVA3_ALLOC_LOG") != nullptr;
    // a fatal error shows its message on screen (the system's error dialog), then the game closes
    rt::set_fatal_notify([](const char *msg) {
        ErrorApplicationConfig c;
        const std::string text = std::string("N.O.V.A. 3 stopped: ") + msg;
        if (R_SUCCEEDED(errorApplicationCreate(&c, text.c_str(), "Details: sdmc:/switch/nova3/nova3.log")))
            errorApplicationShow(&c);
    });
    const AppletType type = appletGetAppletType();
    if (type != AppletType_Application && type != AppletType_SystemApplication)
        rt::log("warning: running as an applet with little memory: start it from a game while holding R");
    socketInitializeDefault();  // local Wi-Fi multiplayer, the game's own networking
    struct stat st;
    // game files on the SD card (the .nro) come first; otherwise the title's RomFS. (Not
    // romfsInit for the .nro: it carries none, and the fallback crashes Ryujinx's file system
    // service, which starts an .nro without the homebrew loader's environment.)
    if (stat((g_data + "/libGloftNAHM.so").c_str(), &st) != 0 && R_SUCCEEDED(romfsInit()) &&
        stat("romfs:/libGloftNAHM.so", &st) == 0)
        g_data = "romfs:";
    rt::log("game files: %s", g_data.c_str());

    static Boot boot;
    struct stat ini;
    const bool first_start = stat((kRoot + "/nova3.ini").c_str(), &ini) != 0;
    boot.cfg = settings::load(kRoot + "/nova3.ini");
    if (first_start) {  // the Switch's own defaults: 60 fps, adaptive VSync (no drop to 30 when late)
        boot.cfg.max_fps = 60;
        boot.cfg.vsync = settings::Settings::VSync::Adaptive;
        settings::save(kRoot + "/nova3.ini", boot.cfg);
    }
    g_boot = &boot;
    g_docked = appletGetOperationMode() == AppletOperationMode_Console;
    input::configure(boot.cfg, kRoot + "/nova3.ini");
    rt::net_set_peers(boot.cfg.peers);
    input::set_screen(boot.width, boot.height);
    audio::set_volume(boot.cfg.volume / 100.0f);

    // Mesa's threaded GL: the driver's work runs on a thread of its own (another core), off
    // the game thread, which is CPU-bound in play (it read its option before the context exists)
    if (boot.cfg.glthread) setenv("mesa_glthread", "true", 1);
    // Mesa offers program binaries (the shader cache, gl_set_shader_cache) only with its disk
    // cache, which finds its folder through these: a Switch has no HOME
    mkdir((kRoot + "/mesa").c_str(), 0777);
    setenv("HOME", kRoot.c_str(), 1);
    setenv("XDG_CACHE_HOME", (kRoot + "/mesa").c_str(), 1);
    setenv("MESA_GLSL_CACHE_DIR", (kRoot + "/mesa").c_str(), 1);
    setenv("MESA_SHADER_CACHE_DIR", (kRoot + "/mesa").c_str(), 1);
    rt::log("video: threaded GL (glthread) %s", boot.cfg.glthread ? "on" : "off");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) rt::fatal("SDL_Init: %s", SDL_GetError());  // audio: libnx AudioOut (host/audio.cpp)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    g_window = SDL_CreateWindow("N.O.V.A. 3", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!g_window) rt::fatal("SDL_CreateWindow: %s", SDL_GetError());
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) rt::fatal("GLES 2 context: %s", SDL_GetError());
    SDL_GL_MakeCurrent(g_window, nullptr);
    set_render_size(boot, g_docked);

    // Recompiled code nests one host frame per guest call: a big stack (rt::os keeps a part).
    if (!rt::os::start_thread(game_thread, &boot, size_t(512) << 20)) rt::fatal("cannot start the game thread");
    // The game thread does everything (input from libnx, HOME / close via appletMainLoop); no
    // SDL event pumping here: an exit from the game thread (a guest exit(), a fatal error)
    // closes libnx's services, and SDL's touch polling would then read freed HID memory.
    while (!boot.done) svcSleepThread(10'000'000);
    // no romfsExit / socketExit: the guest threads are still going (Vox streams music from the
    // RomFS); the process ends and the system releases everything
    rt::os::exit_process(0);
}
