// nova3: host for the recompiled N.O.V.A. 3 library: SDL window and GLES context,
// settings, Android lifecycle replay and the frame loop.
#include "audio.h"
#include "controls_menu.h"
#include "discord.h"
#include "display.h"
#include "texpack.h"
#include "eventlog.h"
#include "fps_overlay.h"
#include "framestats.h"
#include "hooks.h"
#include "input.h"
#include "multi.h"
#include "jni.h"
#include "launcher.h"
#include "os.h"
#include "runtime.h"
#include "settings.h"

#include <SDL3/SDL.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>  // SetDllDirectoryW (ANGLE lookup)
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Options {
    std::string lib = "game/libGloftNAHM.so";
    std::string hd_pack;  // hd_textures.pak next to the game's library
    std::string obb_dir = "com.gameloft.android.ANMP.GloftNAHM";
    std::string data_dir;  // --data: the guest's scratch tree (default: <temp>/nova3, see main)
    std::string apk = "NOVA-3-v1-0-1d.apk";
    int frames = -1;    // frames to run (-1: until the window is closed)
    bool scripted = false;  // set by any scripted-input / screenshot option
    bool play = false;      // --play: a person plays on from --tour-stop (focus, sound, real input)
    std::string user_dir;  // --user: the player's folder (default: user_folder())
    std::string config;    // --config: the settings file (default: <user_dir>/nova3.ini)
    bool size_given = false;  // --size: windowed at that size, whatever the settings say
    bool silent = false;      // -silent: no launcher
    int split = 0;            // --split N[:monitor]: one of N game windows tiled over the monitor (host/multi.cpp)
    int split_display = -1;   // the monitor for --split (-1: [video] display)
    unsigned long long container = 0;  // --split N:monitor:hwnd: the split screen's window (player 1's)
    std::string pads;         // --pads: each player's controller ([launcher] pads)
    std::string names;        // --names: players 2-8's names ([launcher] names; index k for player k + 1)
    std::string screens;      // --screens: players with a window of their own ([launcher] screens)
    int split_fps = 0;        // --fps: split screen: every player's frame limit ([launcher] split_fps)
    int own_fps = -1;         // --own-fps: split screen, a window of its own: player 1's [video] max_fps
    int player = 0;           // --player: split screen: this window's player number, from player 1
    std::string shared_audio; // --shared-audio: the shared window's audio output ([launcher] shared_audio)
    std::string own_audio;    // --own-audio: the own-screen players' outputs ([launcher] own_audio)
    int own_monitor = -1;     // split screen: this player's own window is on this monitor (-1: a view)
    int shared_count = 0, shared_index = 0;  // the players in the shared window, and this one's place (1-based)
    std::string launcher_shot;  // --launcher-shot file.png (debug): render the launcher and exit
    int width = 1280, height = 720;
    settings::Settings cfg;
    bool headless = false;  // null renderer, no window
    std::vector<std::pair<int, std::string>> screenshots;
    std::vector<std::tuple<int, int, int>> taps;  // --tap frame:x,y (repeatable)
    struct Hold { int frame, n; std::vector<int> codes; };
    std::vector<Hold> holds;                         // --hold frame:KEY,frames (KEY: a binding name, e.g. W, Mouse1, Escape)
    std::vector<std::tuple<int, int, int>> looks;    // --look frame:dx,dy (raw mouse counts)
    std::vector<std::pair<int, int>> wheels;         // --wheel frame:steps (+ = down)
    int tour = -1;                                   // --tour chapter: play it through, checkpoint by checkpoint
    int bench_chapter = -1, bench_frames = 0;        // --bench chapter:frames[:walk] (eventlog::bench)
    bool bench_walk = false;
    bool profile = false;                            // --profile: with --bench, the hottest code
    std::string tour_triggers;                       // --tour-triggers object:handler:section,... (tools/tourplan.py)
    std::string tour_shots;                          // --tour-shots dir: a screenshot per checkpoint
    std::string tour_lua;                            // --tour-lua code: run before the --tour-stop checkpoint is saved
    int tour_stop = -1;                              // --tour-stop spawn: stop there; scripted frames count from it
    struct Drag { int frame, x1, y1, x2, y2, n; };
    std::vector<Drag> drags;  // --drag frame:x1,y1,x2,y2,frames (repeatable)
    std::vector<std::pair<int, uint32_t>> peeks;
    std::vector<int> hud_dumps;  // --dump-hud frame (debug)
    std::vector<std::pair<int, std::string>> lua;  // --lua FRAME:code (debug): run a Lua chunk in the level
    struct Place { int frame; float x, y, z; };
    std::vector<Place> places;  // --place FRAME:x,y,z (debug): move the player
    std::vector<std::tuple<int, float, float>> rays;  // --ray FRAME:x,y (debug): vertical physics ray
    std::vector<int> restarts;  // --restart FRAME (debug): restart from the checkpoint, as the pause menu does
    std::vector<int> zone_dumps;  // --zones FRAME (debug): log the level's zones and the player's zone
    std::vector<int> player_logs;  // --log-player FRAME[,EVERY,COUNT] (debug): the player's state
    int enemy_logs = 0;            // --log-enemies EVERY (debug): the enemies the scripts count, from the stop
    std::vector<std::pair<int, std::string>> typed;  // --type frame:text (scripted typing)
    std::string probe;  // comma-separated link addresses: ABI-check these functions and exit
};

// Debug aid: call guest functions with sentinel callee-saved registers and report any the
// callee fails to preserve (AAPCS: r4-r11, sp, d8-d15).
int run_probes(Cpu *cpu, const std::string &list) {
    int bad = 0;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        // entry: addr[/r0/r1/r2/r3] in hex
        std::string item = list.substr(pos, end - pos);
        pos = end + 1;
        uint32_t a[5] = {0, 123456, 0, 1000, 0};
        for (int k = 0; k < 5 && !item.empty(); k++) {
            size_t slash = item.find('/');
            a[k] = uint32_t(std::stoul(item.substr(0, slash), nullptr, 16));
            item = slash == std::string::npos ? "" : item.substr(slash + 1);
        }
        uint32_t link = a[0];
        Cpu before = *cpu;
        for (int i = 4; i <= 11; i++) cpu->r[i] = 0xc0de0000u | uint32_t(i);
        for (int i = 8; i < 16; i++) cpu->vr.x[i] = 0x5eed000000000000ull | uint64_t(i);
        Cpu sent = *cpu;
        rt::call(cpu, rt::sym(link), {a[1], a[2], a[3], a[4]});
        std::string diffs;
        for (int i = 4; i <= 11; i++)
            if (cpu->r[i] != sent.r[i]) diffs += " r" + std::to_string(i);
        if (cpu->r[13] != sent.r[13]) diffs += " sp";
        for (int i = 8; i < 16; i++)
            if (cpu->vr.x[i] != sent.vr.x[i]) diffs += " d" + std::to_string(i);
        rt::log("probe %08x: r0=%08x r1=%08x %s%s", link, cpu->r[0], cpu->r[1], diffs.empty() ? "ABI ok" : "CLOBBERED:", diffs.c_str());
        bad += !diffs.empty();
        *cpu = before;
    }
    return bad ? 1 : 0;
}

Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc;) {
        std::string k = argv[i];
        if (k == "-silent" || k == "--silent") {  // skip the launcher: boot with the saved settings
            o.silent = true;
            i++;
            continue;
        }
        if (i + 1 >= argc) { std::fprintf(stderr, "option %s needs a value\n", k.c_str()); std::exit(2); }
        std::string v = argv[i + 1];
        i += 2;
        if (k == "--lib") o.lib = v;
        else if (k == "--obb") o.obb_dir = v;
        else if (k == "--data") o.data_dir = v;
        else if (k == "--apk") o.apk = v;
        else if (k == "--probe") o.probe = v;
        else if (k == "--headless") { o.headless = v != "0"; }
        else if (k == "--hold") {
            char key[32] = {};
            Options::Hold h{};
            if (std::sscanf(v.c_str(), "%d:%31[^,],%d", &h.frame, key, &h.n) == 3) {
                h.codes = settings::parse_input(key);
                if (h.codes.empty()) { std::fprintf(stderr, "--hold: unknown key %s\n", key); std::exit(2); }
                o.holds.push_back(h);
            }
        }
        else if (k == "--look") {
            int frame, dx, dy;
            if (std::sscanf(v.c_str(), "%d:%d,%d", &frame, &dx, &dy) == 3) o.looks.push_back({frame, dx, dy});
        }
        else if (k == "--tour") o.tour = std::atoi(v.c_str());
        else if (k == "--speed") hooks::g_time_scale = std::clamp(float(std::atof(v.c_str())), 0.1f, 16.0f);
        else if (k == "--tour-triggers") o.tour_triggers = v;
        else if (k == "--bench") {
            std::sscanf(v.c_str(), "%d:%d", &o.bench_chapter, &o.bench_frames);
            o.bench_walk = v.find(":walk") != std::string::npos;
        }
        else if (k == "--profile") o.profile = v != "0";
        else if (k == "--tour-shots") o.tour_shots = v;
        else if (k == "--tour-lua") o.tour_lua = v;
        else if (k == "--tour-stop") o.tour_stop = std::atoi(v.c_str());
        else if (k == "--play") o.play = v != "0";
        else if (k == "--wheel") {
            int frame, steps;
            if (std::sscanf(v.c_str(), "%d:%d", &frame, &steps) == 2) o.wheels.push_back({frame, steps});
        }
        else if (k == "--drag") {
            Options::Drag d{};
            if (std::sscanf(v.c_str(), "%d:%d,%d,%d,%d,%d", &d.frame, &d.x1, &d.y1, &d.x2, &d.y2, &d.n) == 6) o.drags.push_back(d);
        }
        else if (k == "--trace-events") hooks::g_trace_events = v != "0";
        else if (k == "--type") {
            size_t colon = v.find(':');
            o.typed.push_back({std::stoi(v.substr(0, colon)), v.substr(colon + 1)});
        }
        else if (k == "--dump-hud") o.hud_dumps.push_back(std::stoi(v));
        else if (k == "--lua") {
            size_t colon = v.find(':');
            o.lua.push_back({std::stoi(v.substr(0, colon)), v.substr(colon + 1)});
        }
        else if (k == "--restart") o.restarts.push_back(std::stoi(v));
        else if (k == "--zones") o.zone_dumps.push_back(std::stoi(v));
        else if (k == "--log-enemies") o.enemy_logs = std::stoi(v);
        else if (k == "--log-player") {
            int frame = 0, every = 1, count = 1;
            std::sscanf(v.c_str(), "%d,%d,%d", &frame, &every, &count);
            for (int i = 0; i < count; i++) o.player_logs.push_back(frame + i * every);
        }
        else if (k == "--ray") {
            int frame;
            float x, y;
            if (std::sscanf(v.c_str(), "%d:%f,%f", &frame, &x, &y) == 3) o.rays.push_back({frame, x, y});
        }
        else if (k == "--place") {
            Options::Place p{};
            if (std::sscanf(v.c_str(), "%d:%f,%f,%f", &p.frame, &p.x, &p.y, &p.z) == 4) o.places.push_back(p);
        }
        else if (k == "--peek") {
            int frame;
            unsigned addr;
            if (std::sscanf(v.c_str(), "%d:%x", &frame, &addr) == 2) o.peeks.push_back({frame, addr});
        }
        else if (k == "--tap") {
            int frame, x, y;
            if (std::sscanf(v.c_str(), "%d:%d,%d", &frame, &x, &y) == 3) o.taps.push_back({frame, x, y});
        }
        else if (k == "--screenshot") {
            size_t colon = v.find(':');
            o.screenshots.push_back({std::stoi(v.substr(0, colon)), v.substr(colon + 1)});
        }
        else if (k == "--frames") o.frames = std::stoi(v);
        else if (k == "--size") o.size_given = std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) == 2;
        // absolute: the working directory changes to the game files' folder (find_game_files)
        else if (k == "--config") o.config = std::filesystem::absolute(v).string();
        else if (k == "--user") o.user_dir = std::filesystem::absolute(v).string();
        else if (k == "--pads") o.pads = v;
        else if (k == "--names") o.names = v;
        else if (k == "--screens") o.screens = v;
        else if (k == "--fps") o.split_fps = std::atoi(v.c_str());
        else if (k == "--own-fps") o.own_fps = std::atoi(v.c_str());
        else if (k == "--player") o.player = std::atoi(v.c_str());
        else if (k == "--shared-audio") o.shared_audio = v;
        else if (k == "--own-audio") o.own_audio = v;
        else if (k == "--split") {
            if (std::sscanf(v.c_str(), "%d:%d:%llx", &o.split, &o.split_display, &o.container) < 1) o.split = 0;
            o.split = std::clamp(o.split, 0, 8);
        }
        else if (k == "--launcher-shot") o.launcher_shot = v;
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); std::exit(2); }
    }
    o.scripted = !o.taps.empty() || !o.typed.empty() || !o.drags.empty() || !o.holds.empty() || !o.looks.empty() ||
                 !o.screenshots.empty() || o.tour >= 0 || o.bench_chapter >= 0;
    return o;
}

// Frame limiter ([video] max_fps): waits on a high-resolution timer until the next frame's
// deadline. Falling more than a frame behind resets the schedule instead of catching up.
void limit_frame_rate(int max_fps) {
    if (max_fps <= 0) return;
    static int64_t next = 0;
    const int64_t period = 1000000000 / max_fps;
    int64_t now = rt::os::monotonic_ns();
    next = (next == 0 || now - next > period) ? now + period : next + period;
    if (now < next) rt::os::sleep_ns(next - now);
}

SDL_Window *g_window;
SDL_GLContext g_gl;

// --- video settings while running (display::apply, the in-game Graphics page) ----------------

// The closest exclusive fullscreen mode to w x h @ refresh on `display` (refresh 0: the
// highest rate at that resolution).
bool fullscreen_mode(SDL_DisplayID display, int w, int h, int refresh, SDL_DisplayMode &m) {
    if (!SDL_GetClosestFullscreenDisplayMode(display, w, h, float(refresh), true, &m)) return false;
    if (refresh == 0) {
        int n = 0;
        if (SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &n)) {
            for (int i = 0; i < n; i++)
                if (modes[i]->w == m.w && modes[i]->h == m.h && modes[i]->refresh_rate > m.refresh_rate) m = *modes[i];
            SDL_free(modes);
        }
    }
    return true;
}

struct Video {
    // the window's display, read on the main thread when the window is created
    int desktop_w = 1920, desktop_h = 1080;
    std::vector<std::pair<int, int>> fullscreen_sizes;  // exclusive modes, largest first
    // what is applied (game thread); the display mode and size as the settings say, also in
    // test runs (whose window is always a plain one): only a change on the Graphics page acts
    settings::DisplayMode mode = settings::DisplayMode::Windowed;
    int width = 0, height = 0, refresh = 0;
    settings::Settings::VSync vsync = settings::Settings::VSync::On;
    int anisotropy = 0, fov = -1;
    bool hd_textures = false;  // as the game started (a change needs a restart)
};
Video g_video;
// the window's new size in pixels, from the main thread: (width << 32) | height, 0 = none
std::atomic<uint64_t> g_new_size{0};

void note_window_size(int w, int h) {
    if (w > 0 && h > 0) g_new_size = (uint64_t(uint32_t(w)) << 32) | uint32_t(h);
}

// Game thread (the GL context): VSync, with adaptive falling back to plain VSync.
void apply_vsync(settings::Settings::VSync vs) {
    using VSync = settings::Settings::VSync;
    if (vs == VSync::Adaptive && !SDL_GL_SetSwapInterval(-1)) {
        rt::log("adaptive vsync unsupported (%s); using vsync", SDL_GetError());
        vs = VSync::On;
    }
    if (vs != VSync::Adaptive) SDL_GL_SetSwapInterval(vs == VSync::On ? 1 : 0);
}

// [video] fov: horizontal degrees at 16:9 -> vertical, relative to the game's own view
// (fovy 0.6024 rad, about 58 degrees horizontal at 16:9); 0 = the game's own.
float fov_scale(int fov) {
    if (fov <= 0) return 1.0f;
    const double h = fov * 3.14159265358979 / 180, v = 2 * std::atan(std::tan(h / 2) * 9.0 / 16.0);
    return float(v / 0.60238);
}

bool g_embedded;  // split screen: this player's view is a child of player 1's window (fixed size)

// Main thread: the window per display mode and size (exclusive: the closest mode).
struct WindowRequest {
    settings::DisplayMode mode;
    int width, height, refresh;
};
void SDLCALL change_window(void *param) {
    const WindowRequest r = *static_cast<WindowRequest *>(param);
    delete static_cast<WindowRequest *>(param);
    if (g_embedded) return;  // a split-screen view keeps its place in the shared window
    using settings::DisplayMode;
    SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
    if (r.mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SyncWindow(g_window);
        SDL_SetWindowSize(g_window, r.width, r.height);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    } else {
        SDL_DisplayMode m{};
        const bool exclusive = r.mode == DisplayMode::Fullscreen && fullscreen_mode(display, r.width, r.height, r.refresh, m);
        SDL_SetWindowFullscreenMode(g_window, exclusive ? &m : nullptr);  // null: borderless at the desktop's size
        SDL_SetWindowFullscreen(g_window, true);
        if (exclusive) rt::log("display mode: %dx%d @ %g Hz", m.w, m.h, double(m.refresh_rate));
    }
    SDL_SyncWindow(g_window);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    note_window_size(w, h);
    rt::log("window: %s, %dx%d", r.mode == DisplayMode::Windowed ? "windowed" : r.mode == DisplayMode::Borderless ? "borderless" : "fullscreen", w, h);
}

// Replays what the Java side (GL2JNIActivity / GL2JNIView) does on Android.
void boot_activity(Options &opt) {
    const std::string pkg = "com/gameloft/android/ANMP/GloftNAHM/";
    const std::string jpkg = "Java_com_gameloft_android_ANMP_GloftNAHM_";
    auto native = [&](const std::string &cls, const std::string &method, std::initializer_list<uint32_t> args = {}) {
        std::string jcls = cls;
        for (char &ch : jcls) if (ch == '/') ch = '_';
        rt::log("-> %s.%s", cls.c_str(), method.c_str());
        return jni::call_native((jpkg + jcls + "_" + method).c_str(), pkg + cls, args);
    };
    jni::set_screen_size(opt.width, opt.height);
    jni::init();
    input::register_java();
    audio::register_java();
    if (uint32_t onload = rt::export_address("JNI_OnLoad")) {
        uint32_t version = rt::call(rt::current_cpu(), onload, {jni::vm(), 0});
        rt::log("JNI_OnLoad -> %08x", version);
        audio::select_backend();  // after JNI_OnLoad, which sets Vox's Android API level
    }
    native("GLUtils/SUtils", "nativeInit");
    native("GLUtils/Device", "nativeInit");
    native("SendInfo", "initMethods");
    native("DataSharing", "nativeInit");
    native("PushNotification/SimplifiedAndroidUtils", "nativeInit");
    // GLSocialLib Java classes register their native side when initialised at startup.
    auto social = [&](const std::string &cls) {
        std::string jcls = cls;
        for (char &ch : jcls) if (ch == '/') ch = '_';
        rt::log("-> %s.nativeInit", cls.c_str());
        jni::call_native(("Java_com_gameloft_" + jcls + "_nativeInit").c_str(), "com/gameloft/" + cls, {});
    };
    social("GLSocialLib/PlatformAndroid");
    social("GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib");
    social("GLSocialLib/facebook/FacebookAndroidGLSocialLib");
    native("GL2JNILib", "init");  // GameInitialize (onResume)
    // GL thread: EGL config chosen -> initGL; renderer start -> nativeInit; surface size -> resize
    native("GL2JNILib", "initGL");
    native("GL2JNILib", "InitViewSettings");
    native("GL2JNIActivity", "nativeInit");
    native("GL2JNILib", "resize", {uint32_t(opt.width), uint32_t(opt.height)});
    // GameResume is only sent when returning from a pause (Renderer.a), not on first start.
    const std::string lib = jpkg + "GL2JNILib_";
    bool in_game = false;
    FrameStats stats;
    const bool tour_stop = opt.tour >= 0 && opt.tour_stop >= 0;
    for (int f = 0; opt.frames < 0 || tour_stop || f < opt.frames; f++) {
        // the scripted options' frame (with --tour-stop counted from the stop, -1 before it;
        // --frames too)
        const int sf = eventlog::script_frame(f, tour_stop);
        if (tour_stop && opt.frames >= 0 && sf >= opt.frames) break;
        const int64_t step_start = stats.mark();
        if (f % 300 == 0) {
            if (f) stats.log(f);
            else rt::log("frame 0");
        }
        for (auto &[frame, x, y] : opt.taps) {  // scripted taps: press at `frame`, release 3 frames later
            if (frame == sf) input::post({input::Event::Touch, 1, x, y, 0});
            if (frame + 3 == sf) input::post({input::Event::Touch, 0, x, y, 0});
        }
        for (const auto &d : opt.drags) {  // press, move linearly over n frames, release
            int k = sf - d.frame;
            if (k < 0 || k > d.n + 1) continue;
            int t = std::min(k, d.n);
            int x = d.x1 + (d.x2 - d.x1) * t / std::max(d.n, 1), y = d.y1 + (d.y2 - d.y1) * t / std::max(d.n, 1);
            input::post({input::Event::Touch, k == 0 ? 1 : k == d.n + 1 ? 0 : 2, x, y, 0});
        }
        for (auto &[frame, text] : opt.typed)
            if (frame == sf) input::type_text(text + std::string(1, char(13)));  // Enter finishes the entry
        // The window's size changed (the Graphics page: display mode, resolution): the game
        // renders at the new size, told as Android tells it a new surface size.
        if (const uint64_t size = g_new_size.exchange(0)) {
            const int w = int(size >> 32), h = int(uint32_t(size));
            if (w != opt.width || h != opt.height) {
                rt::log("window resized: %dx%d -> %dx%d", opt.width, opt.height, w, h);
                opt.width = w;
                opt.height = h;
                jni::set_screen_size(w, h);
                native("GL2JNILib", "resize", {uint32_t(w), uint32_t(h)});
            }
        }
        input::update();
        for (const input::Event &e : input::drain()) {
            if (e.kind == input::Event::Touch)
                jni::call_native((lib + "touchEvent").c_str(), pkg + "GL2JNILib",
                                 {uint32_t(e.a), uint32_t(e.b), uint32_t(e.c), uint32_t(e.d)});
            else if (e.kind == input::Event::Text)  // Android's VirtualKeyboard.onTextChanged
                jni::call_native((jpkg + "GLUtils_SUtils_nativeSendKeyboardData").c_str(), pkg + "GLUtils/SUtils",
                                 {jni::new_string(e.text)});
            else
                jni::call_native((lib + (e.a ? "OnKeyDown" : "OnKeyUp")).c_str(), pkg + "GL2JNILib", {uint32_t(e.b)});
        }
        // Gameplay (no touch input, captured mouse) while the in-game state was active last
        // frame; menus, briefings and loading screens keep mouse-as-touch.
        input::set_gameplay(in_game);
        for (const auto &h : opt.holds) {
            for (int code : h.codes) {
                if (h.frame == sf) input::script_input(code, true);
                if (h.frame + h.n == sf) input::script_input(code, false);
            }
        }
        for (auto &[frame, dx, dy] : opt.looks)
            if (frame == sf) input::script_look(dx, dy);
        for (auto &[frame, steps] : opt.wheels)
            if (frame == sf) input::script_wheel(steps);
        // outside gameplay an idle control state releases anything still held
        if (input::gameplay()) hooks::apply_controls(rt::current_cpu(), input::take_controls(), {});
        else hooks::apply_controls(rt::current_cpu(), {}, input::take_menu_nav());
        eventlog::frame(rt::current_cpu());
        hooks::auto_continue_loading(rt::current_cpu());
        hooks::watch_section_fall(rt::current_cpu());
        hooks::kill_fallen_enemies(rt::current_cpu());
        controls_menu::frame(rt::current_cpu());
        if (opt.tour >= 0)
            eventlog::tour(rt::current_cpu(), opt.tour, opt.tour_triggers, opt.tour_shots, opt.width, opt.height, opt.tour_stop,
                           opt.tour_lua);
        if (opt.bench_chapter >= 0) eventlog::bench(rt::current_cpu(), opt.bench_chapter, opt.bench_frames, opt.profile, opt.bench_walk);
        discord::frame(rt::current_cpu());
        jni::call_native((lib + "step").c_str(), pkg + "GL2JNILib", {});
        in_game = input::consume_ingame_frame();
        rt::watchdog_frame();
        for (int frame : opt.hud_dumps)
            if (frame == sf) hooks::dump_hud(rt::current_cpu());
        for (auto &[frame, code] : opt.lua)
            if (frame == sf) eventlog::run_lua(rt::current_cpu(), code.c_str());
        for (auto &[frame, x, y] : opt.rays)
            if (frame == sf) eventlog::ray_down(rt::current_cpu(), x, y);
        for (int frame : opt.restarts)
            if (frame == sf) eventlog::restart_checkpoint(rt::current_cpu());
        for (int frame : opt.zone_dumps)
            if (frame == sf) eventlog::dump_zones(rt::current_cpu());
        for (int frame : opt.player_logs)
            if (frame == sf) eventlog::log_player(rt::current_cpu());
        if (opt.enemy_logs && sf >= 0) eventlog::log_enemies(rt::current_cpu(), sf, opt.enemy_logs);
        for (auto &p : opt.places)
            if (p.frame == sf) eventlog::place_player(rt::current_cpu(), p.x, p.y, p.z);
        for (auto &[frame, addr] : opt.peeks) {
            if (frame != sf) continue;
            uint32_t v = LD32(rt::sym(addr));
            rt::log("peek %08x = %08x -> %08x", addr, v, v >= 0x1000 ? LD32(v) : 0);
        }
        if (opt.headless) continue;
        if (opt.cfg.fps_counter) {
            int fw = 0, fh = 0;
            SDL_GetWindowSizeInPixels(g_window, &fw, &fh);
            fps_overlay::draw(fw, fh);
        }
        for (auto &[frame, path] : opt.screenshots)
            if (frame == sf && rt::gl_screenshot(path.c_str(), opt.width, opt.height)) rt::log("screenshot: %s", path.c_str());
        const int64_t swap_start = stats.mark();
        SDL_GL_SwapWindow(g_window);
        limit_frame_rate(opt.play && sf >= 0 && opt.cfg.max_fps <= 0 ? 60 : opt.cfg.max_fps);  // (a tour runs unlimited)
        stats.add(step_start, swap_start, stats.mark());
    }
    rt::log("ran %d frames", opt.frames);
}

struct Boot {
    Options opt;
    std::atomic<bool> done{false};
    int result = 1;
};
Boot *g_boot;  // display::*

void game_thread(void *param) {
    auto *boot = static_cast<Boot *>(param);
    rt::init_memory();
    rt::install_crash_handler();
    rt::start_watchdog(10);
    rt::FsConfig fs;
    fs.data_dir = boot->opt.data_dir;
    fs.save_dir = (std::filesystem::path(boot->opt.user_dir) / "save").string();
    fs.obb_dir = boot->opt.obb_dir;
    fs.apk_path = boot->opt.apk;
    rt::init_fs(fs);

    static rt::LoadedLib lib = rt::load_library(boot->opt.lib.c_str());
    rt::set_main_library(&lib);
    rt::set_crash_library(&lib);
    Cpu *cpu = rt::init_main_thread();
    rt::log("loaded %s: %08x-%08x, %u static constructors", boot->opt.lib.c_str(), lib.base, lib.end, lib.init_array_count);
    if (!boot->opt.probe.empty()) {
        boot->result = run_probes(cpu, boot->opt.probe);
    } else {
        for (uint32_t i = 0; i < lib.init_array_count; i++) {
            uint32_t fn = LD32(lib.init_array + 4 * i);
            if (fn == 0 || fn == 0xffffffffu) continue;
            rt::call(cpu, fn, {});
        }
        rt::log("static constructors done");
        if (boot->opt.headless) {
            rt::install_null_gl();
        } else {
            // The context was created on the main thread (with the window) and released there.
            if (!SDL_GL_MakeCurrent(g_window, g_gl)) rt::fatal("SDL_GL_MakeCurrent: %s", SDL_GetError());
            const settings::Settings &cfg = boot->opt.cfg;
            apply_vsync(cfg.vsync);
            rt::gl_set_shader_cache((std::filesystem::path(boot->opt.user_dir) / "shadercache").string());
            rt::gl_set_anisotropy(float(cfg.anisotropy));
            hooks::g_fov_scale = fov_scale(cfg.fov);
            if (cfg.fov > 0) rt::log("field of view: %d degrees (x%.3f)", cfg.fov, double(hooks::g_fov_scale));
            g_video.mode = cfg.mode;
            g_video.width = cfg.width;
            g_video.height = cfg.height;
            g_video.refresh = cfg.refresh;
            g_video.vsync = cfg.vsync;
            g_video.anisotropy = cfg.anisotropy;
            g_video.fov = cfg.fov;
            g_video.hd_textures = cfg.hd_textures;
            if (!rt::gl_load([](const char *name) { return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name)); }))
                rt::fatal("missing GLES2 functions");
            if (boot->opt.cfg.hd_textures) texpack::init(boot->opt.hd_pack);
            if (!boot->opt.profile) rt::os::start_profiler_continuous(30);  // the log's "profile:" lines
        }
        boot_activity(boot->opt);
        boot->result = 0;
    }
    boot->done = true;
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
}

#ifdef _WIN32
// Windows has no GLES driver: SDL uses ANGLE (GLES on Direct3D 11), from libEGL.dll /
// libGLESv2.dll next to the executable or else from a Microsoft Edge installation.
void use_angle() {
    namespace fs = std::filesystem;
    fs::path dir = SDL_GetBasePath();
    if (!fs::exists(dir / "libGLESv2.dll") || !fs::exists(dir / "libEGL.dll")) {
        fs::path best;
        for (const char *root : {"C:/Program Files (x86)/Microsoft/Edge/Application", "C:/Program Files/Microsoft/Edge/Application"}) {
            std::error_code ec;
            for (auto &e : fs::directory_iterator(root, ec))
                if (fs::exists(e.path() / "libGLESv2.dll") && (best.empty() || e.path().filename() > best.filename()))
                    best = e.path();
        }
        if (best.empty()) rt::fatal("ANGLE not found: put libEGL.dll and libGLESv2.dll next to nova3.exe");
        dir = best;
    }
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");  // GLES through EGL instead of WGL
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_SetHint(SDL_HINT_EGL_LIBRARY, (dir / "libEGL.dll").string().c_str());
    // SDL loads the GLES library by name: let the DLL search find ANGLE's.
    SetDllDirectoryW(dir.wstring().c_str());
    rt::log("ANGLE from %s", dir.string().c_str());
}
#endif

// Window per [video]: windowed (width x height), borderless fullscreen at the desktop
// resolution, or exclusive fullscreen (closest display mode to width x height @ refresh), on
// the chosen monitor; with a GLES 2 context that the game thread makes current. Tests
// (scripted / --size) always get a plain window of the requested size.
// [launcher] windows = N: start N - 1 more nova3.exe, straight into the game, each taking the
// next player slot, its own controller and its tile (create_window). No inherited output: each
// writes its own log (nova3-playerN.log).
void open_other_windows(const Options &opt) {
    const char *base = SDL_GetBasePath();
#ifdef _WIN32
    const std::string exe = std::string(base ? base : "") + "nova3.exe";
#else
    const std::string exe = std::string(base ? base : "") + "nova3";
#endif
    char split[64];
    std::snprintf(split, sizeof split, "%d:%d:%llx", opt.split, opt.split_display, opt.container);
    const std::string fps = std::to_string(opt.split_fps), own_fps = std::to_string(opt.cfg.max_fps);
    for (int i = 1; i < opt.split; i++) {
        const std::string player = std::to_string(i + 1);  // each window its player number, whatever slots are free
        const char *args[] = {exe.c_str(), "-silent", "--player", player.c_str(), "--split", split, "--fps", fps.c_str(),
                              "--own-fps", own_fps.c_str(),
                              "--pads", opt.pads.c_str(),
                              "--names", opt.names.c_str(), "--screens", opt.screens.c_str(),
                              "--shared-audio", opt.shared_audio.c_str(), "--own-audio", opt.own_audio.c_str(), nullptr};
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_Process *p = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (p) SDL_DestroyProcess(p);  // only the handle: the window keeps running
        else rt::log("cannot open game window %d: %s", i + 1, SDL_GetError());
    }
    rt::log("split: %d players", opt.split);
}

// Split screen: who shares the window and who has one of their own ([launcher] screens).
void split_layout(Options &opt) {
    const std::vector<std::string> screens = multi::split_list(opt.screens);
    auto own = [&](int player) { return size_t(player) <= screens.size() && !screens[size_t(player - 1)].empty(); };
    opt.shared_count = opt.shared_index = 0;
    for (int p = 1; p <= opt.split; p++) {
        if (own(p)) continue;
        opt.shared_count++;
        if (p == multi::slot()) opt.shared_index = opt.shared_count;
    }
    opt.own_monitor = own(multi::slot()) ? std::max(0, std::atoi(screens[size_t(multi::slot() - 1)].c_str())) : -1;
}

// Split screen: player 1's one window, split into a view per player (every player's game
// window becomes a child of it: host/multi.cpp). Windowed: a normal window at the resolution;
// borderless or fullscreen: the whole monitor, over the taskbar.
SDL_Window *g_container;
void create_container(Options &opt) {
    using settings::DisplayMode;
    const settings::Settings &cfg = opt.cfg;
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    SDL_DisplayID display = ids && count ? ids[std::clamp(cfg.display, 0, count - 1)] : SDL_GetPrimaryDisplay();
    SDL_free(ids);
    const bool windowed = cfg.mode == DisplayMode::Windowed;
    SDL_Rect r{0, 0, cfg.width, cfg.height};
    if (!windowed) SDL_GetDisplayBounds(display, &r);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "N.O.V.A. 3");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, r.w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, r.h);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    if (!windowed) SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    SDL_SetHint(SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE, "always");  // black around the letterboxed views
    g_container = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_container) {
        rt::log("split screen window: %s; separate windows instead", SDL_GetError());
        return;
    }
    if (!windowed) SDL_SetWindowFullscreen(g_container, true);  // the desktop's size: covers the taskbar
    SDL_ShowWindow(g_container);
    SDL_SyncWindow(g_container);
#ifdef _WIN32
    opt.container = (unsigned long long)(uintptr_t)SDL_GetPointerProperty(SDL_GetWindowProperties(g_container),
                                                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    rt::log("split screen: %d players in one %s window", opt.split, windowed ? "windowed" : "fullscreen");
}

void *window_hwnd(SDL_Window *w) {
#ifdef _WIN32
    return w ? SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
#else
    (void)w;
    return nullptr;
#endif
}

void create_window(Options &opt) {
#ifdef _WIN32
    use_angle();
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    using settings::DisplayMode;
    const settings::Settings &cfg = opt.cfg;
    const bool test = (opt.scripted && !opt.play) || opt.size_given;
    const bool own = opt.split > 1 && opt.own_monitor >= 0;  // split screen, but a window of its own
    const bool split = opt.split > 1 && !own;  // a view of the split screen: placed in the shared window
    const DisplayMode mode = test || split ? DisplayMode::Windowed : cfg.mode;
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    const int want = own ? opt.own_monitor : split && opt.split_display >= 0 ? opt.split_display : cfg.display;
    SDL_DisplayID display = ids && count ? ids[std::clamp(want, 0, count - 1)] : SDL_GetPrimaryDisplay();
    SDL_free(ids);
    int x = SDL_WINDOWPOS_CENTERED_DISPLAY(display), y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
    void *container = split && opt.container ? (void *)(uintptr_t)opt.container : nullptr;
    if (container && multi::window_alive(container)) {
        // this player's view of the split screen's window: placed and shown by multi::embed
        multi::tile_rect(container, opt.shared_count, opt.shared_index, x, y, opt.width, opt.height);
        rt::log("view: player %d, view %d of %d in the shared window (%dx%d at %d,%d)", multi::slot(), opt.shared_index,
                opt.shared_count, opt.width, opt.height, x, y);
    } else if (split) {
        container = nullptr;
        // the monitor's usable area (without the taskbar) in equal tiles; player k takes tile k
        SDL_Rect r{};
        if (!SDL_GetDisplayUsableBounds(display, &r)) SDL_GetDisplayBounds(display, &r);
        int cols, rows;
        multi::grid(opt.split, cols, rows);
        const int tile = (multi::slot() - 1) % (cols * rows);
        opt.width = r.w / cols;
        opt.height = r.h / rows;
        x = r.x + (tile % cols) * opt.width;
        y = r.y + (tile / cols) * opt.height;
        rt::log("window: tile %d of %d (%dx%d at %d,%d)", tile + 1, opt.split, opt.width, opt.height, x, y);
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    const std::string title = multi::slot() > 1 ? "N.O.V.A. 3 - Player " + std::to_string(multi::slot()) : "N.O.V.A. 3";
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    if (split) SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, opt.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, opt.height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    if (opt.scripted && !opt.play) SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);  // tests must not steal focus
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_window) rt::fatal("SDL_CreateWindow: %s", SDL_GetError());
    if (container) {
        g_embedded = multi::embed(window_hwnd(g_window), container, x, y, opt.width, opt.height);
        if (!g_embedded) rt::log("view: cannot join the split screen's window: a window of its own");
    }
    if (mode == DisplayMode::Fullscreen) {
        SDL_DisplayMode m{};
        if (fullscreen_mode(display, opt.width, opt.height, cfg.refresh, m)) {
            SDL_SetWindowFullscreenMode(g_window, &m);
            rt::log("display mode: %dx%d @ %g Hz", m.w, m.h, double(m.refresh_rate));
        } else {
            rt::log("no fullscreen mode near %dx%d: using borderless", opt.width, opt.height);
        }
    }
    // the display's modes, for the Graphics page (display::resolutions)
    if (const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(display)) g_video.desktop_w = dm->w, g_video.desktop_h = dm->h;
    int n = 0;
    if (SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &n)) {
        for (int i = 0; i < n; i++) {
            const std::pair<int, int> wh{modes[i]->w, modes[i]->h};
            if (std::find(g_video.fullscreen_sizes.begin(), g_video.fullscreen_sizes.end(), wh) == g_video.fullscreen_sizes.end())
                g_video.fullscreen_sizes.push_back(wh);
        }
        SDL_free(modes);
    }
    if (g_video.fullscreen_sizes.empty()) g_video.fullscreen_sizes.push_back({g_video.desktop_w, g_video.desktop_h});
    if (mode != DisplayMode::Windowed) SDL_SetWindowFullscreen(g_window, true);
    if (g_embedded) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");  // a view: the shared window stays in front
        SDL_ShowWindow(g_window);
    } else {
        SDL_ShowWindow(g_window);
        SDL_SyncWindow(g_window);
    }
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) rt::fatal("GLES 2 context: %s", SDL_GetError());
    SDL_GL_MakeCurrent(g_window, nullptr);
    SDL_GetWindowSizeInPixels(g_window, &opt.width, &opt.height);  // the game renders at this size
}

}  // namespace

#ifdef _WIN32
// nova3.exe is a GUI program: started from Explorer it has no console. Log to nova3.log then,
// or to a console window when [launcher] console = true. Runs with redirected output (tests,
// shells) keep their stderr.
bool has_stderr() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    const DWORD type = GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN) return false;
    // NUL is a character device like a console, but nothing reads it (the other game windows
    // of a split start with NUL): log to a file then
    DWORD console_mode;
    return type != FILE_TYPE_CHAR || GetConsoleMode(h, &console_mode);
}
bool g_log_redirected;  // stderr was reopened by open_log (no stderr from the parent)
void open_log(bool console) {
    if (!g_log_redirected && has_stderr()) return;
    if (g_log_redirected) std::fclose(stderr);
    g_log_redirected = true;
    if (console && AllocConsole()) {
        std::freopen("CONOUT$", "w", stderr);
        std::freopen("CONOUT$", "w", stdout);
        SetConsoleTitleW(L"N.O.V.A. 3 log");
        return;
    }
    // next to nova3.exe (the working directory becomes the game files' folder)
    const char *base = SDL_GetBasePath();
    // a second game window (host/multi.cpp) logs to nova3-player2.log, ...
    // (another copy holding this player's slot: its own log name too)
    std::string name = multi::slot() > 1 ? "nova3-player" + std::to_string(multi::slot()) : "nova3";
    if (multi::held_slot() != multi::slot()) name += "-slot" + std::to_string(multi::held_slot());
    name += ".log";
    std::freopen((std::string(base ? base : "") + name).c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}
#else
void open_log(bool) {}
#endif

// The game files are looked up relative to the working directory, which becomes the folder
// that has them:
//  * nova3_data next to nova3.exe (a packaged copy, tools/package.py): libGloftNAHM.so and
//    the OBBs' contents (or the OBB files), all in that one folder;
//  * otherwise the development tree, in the executable's folder or a parent: game/
//    libGloftNAHM.so, the OBB folder named after the package, the APK.
// Options given on the command line are kept, relative to that folder.
void find_game_files(Options &opt) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::path(opt.lib).is_absolute() || fs::exists(opt.lib, ec)) return;
    const char *base = SDL_GetBasePath();
    if (!base) return;
    const fs::path packaged = fs::path(base) / "nova3_data";
    if (fs::exists(packaged / "libGloftNAHM.so", ec)) {
        fs::current_path(packaged, ec);
        if (opt.lib == Options().lib) opt.lib = "libGloftNAHM.so";
        if (opt.obb_dir == Options().obb_dir) opt.obb_dir = ".";
        if (!ec) rt::log("game files: %s", packaged.string().c_str());
        return;
    }
    for (fs::path dir = base; !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / opt.lib, ec)) {
            fs::current_path(dir, ec);
            if (!ec) rt::log("game files: %s", dir.string().c_str());
            return;
        }
        if (dir == dir.parent_path()) break;
    }
}

// The player's own files: the save games (save/: the app's private data, files/ and
// shared_prefs/) and the settings (nova3.ini). Windows: "Saved Games/N.O.V.A. 3"; elsewhere
// the SDL preferences folder (~/.local/share/nova3).
std::string user_folder() {
    namespace fs = std::filesystem;
    if (const char *saved = SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES)) return (fs::path(saved) / "N.O.V.A. 3").string();
    if (char *pref = SDL_GetPrefPath(nullptr, "nova3")) {
        std::string dir = pref;
        SDL_free(pref);
        return dir;
    }
    return "user";  // next to the game files
}

// Earlier builds kept nova3.ini and the saves in the game folder (nova3.ini,
// userdata/root/data/data/<package>). They are copied over once, while the user folder has
// none; the old files stay where they are.
void adopt_old_files(const Options &opt) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path user = opt.user_dir;
    if (opt.config == (user / "nova3.ini").string() && !fs::exists(opt.config, ec) && fs::exists("nova3.ini", ec)) {
        fs::copy_file("nova3.ini", opt.config, ec);
        rt::log("settings: copied nova3.ini to %s%s", opt.config.c_str(), ec ? " (failed)" : "");
    }
    // the development tree's old scratch folder held the saves then
    const fs::path old_save = fs::path("userdata") / "root/data/data/com.gameloft.android.ANMP.GloftNAHM";
    const fs::path save = user / "save";
    if (fs::exists(save, ec) || !fs::exists(old_save / "files", ec)) return;
    fs::create_directories(save, ec);
    for (const char *sub : {"files", "shared_prefs"}) {  // not cache/
        if (!fs::exists(old_save / sub, ec)) continue;
        fs::copy(old_save / sub, save / sub, fs::copy_options::recursive, ec);
        if (ec) rt::log("save: copying %s failed: %s", sub, ec.message().c_str());
    }
    rt::log("save: copied the saves from %s to %s", old_save.string().c_str(), save.string().c_str());
}

// The in-game Graphics page (host/display.h), on the game thread. VSync, the frame cap, the
// field of view and anisotropic filtering apply at once (anisotropy to the textures loaded from
// then on); a display mode or resolution change goes to the main thread (the window's owner),
// and the new size reaches the game at the next frame (boot_activity). HD textures need a
// restart.
namespace display {

bool apply(const settings::Settings &cfg) {
    if (!g_boot || g_boot->opt.headless) return false;
    Options &opt = g_boot->opt;
    using settings::DisplayMode;
    const bool window_changed = cfg.mode != g_video.mode || cfg.refresh != g_video.refresh ||
                                (cfg.mode != DisplayMode::Borderless && (cfg.width != g_video.width || cfg.height != g_video.height));
    if (window_changed) {
        g_video.mode = cfg.mode;
        g_video.width = cfg.width;
        g_video.height = cfg.height;
        g_video.refresh = cfg.refresh;
        auto *request = new WindowRequest{cfg.mode, cfg.width, cfg.height, cfg.refresh};
        if (!SDL_RunOnMainThread(change_window, request, false)) {
            rt::log("display: cannot change the window: %s", SDL_GetError());
            delete request;
        }
    }
    if (cfg.vsync != g_video.vsync) {
        g_video.vsync = cfg.vsync;
        apply_vsync(cfg.vsync);
    }
    if (cfg.anisotropy != g_video.anisotropy) {
        g_video.anisotropy = cfg.anisotropy;
        rt::gl_set_anisotropy(float(cfg.anisotropy));
    }
    if (cfg.fov != g_video.fov) {
        g_video.fov = cfg.fov;
        hooks::g_fov_scale = fov_scale(cfg.fov);
    }
    opt.cfg.mode = cfg.mode;
    opt.cfg.width = cfg.width;
    opt.cfg.height = cfg.height;
    opt.cfg.refresh = cfg.refresh;
    opt.cfg.vsync = cfg.vsync;
    opt.cfg.max_fps = cfg.max_fps;  // limit_frame_rate, every frame
    opt.cfg.anisotropy = cfg.anisotropy;
    opt.cfg.fov = cfg.fov;
    opt.cfg.hd_textures = cfg.hd_textures;
    static const char *const kModes[] = {"windowed", "borderless", "fullscreen"};
    static const char *const kVSync[] = {"off", "on", "adaptive"};
    rt::log("display: %s %dx%d, vsync %s, max fps %d, anisotropy %dx, fov %d, hd textures %s", kModes[int(cfg.mode)],
            cfg.width, cfg.height, kVSync[int(cfg.vsync)], cfg.max_fps, cfg.anisotropy, cfg.fov,
            cfg.hd_textures ? "on" : "off");
    return cfg.hd_textures == g_video.hd_textures;
}

// Windowed: common 16:9 sizes and the display's modes that fit on the desktop (as the
// launcher offers); exclusive fullscreen: the display's modes; borderless: the desktop's size.
// Largest first, with the current size.
std::vector<std::pair<int, int>> resolutions() {
    using settings::DisplayMode;
    std::vector<std::pair<int, int>> out;
    if (g_video.mode == DisplayMode::Borderless) return {{g_video.desktop_w, g_video.desktop_h}};
    std::vector<std::pair<int, int>> v;
    if (g_video.mode == DisplayMode::Windowed) v = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    v.insert(v.end(), g_video.fullscreen_sizes.begin(), g_video.fullscreen_sizes.end());
    for (auto wh : v) {
        if (g_video.mode == DisplayMode::Windowed && (wh.first > g_video.desktop_w || wh.second > g_video.desktop_h)) continue;
        if (std::find(out.begin(), out.end(), wh) == out.end()) out.push_back(wh);
    }
    const std::pair<int, int> current{g_video.width, g_video.height};
    if (current.first > 0 && std::find(out.begin(), out.end(), current) == out.end()) out.push_back(current);
    std::sort(out.begin(), out.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
    return out;
}

void save(const settings::Settings &cfg) {
    if (&cfg == &input::config()) input::save_config();  // the live settings (bindings as text too)
    else if (g_boot) settings::save(g_boot->opt.config, cfg);
}

}  // namespace display

int main(int argc, char **argv) {
    static Boot boot{parse(argc, argv)};
    g_boot = &boot;
    Options &opt = boot.opt;
    // Errors must be visible even without a console (double-click start); scripted test
    // runs must not block on a dialog.
    if (!opt.scripted)
        rt::set_fatal_notify([](const char *msg) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "N.O.V.A. 3", msg, nullptr);
        });
    find_game_files(opt);
    {
        std::error_code ec;
        opt.hd_pack = (std::filesystem::absolute(opt.lib, ec).parent_path() / "hd_textures.pak").string();
        launcher::set_hd_textures_found(std::filesystem::exists(opt.hd_pack, ec));
    }
    multi::claim_slot(opt.player);  // player 1, 2, ... when several game windows are open
    if (opt.player > 0) multi::set_player(opt.player);  // split screen: the number player 1 gave
    open_log(false);  // until the settings say otherwise
    rt::log("N.O.V.A. 3 for Windows, built %s %s", __DATE__, __TIME__);
    if (multi::slot() > 1) rt::log("player %d (another game window is open)", multi::slot());
    // The guest's scratch tree (/sdcard, cache, /tmp, and the /proc and /sys files the runtime
    // generates at every start): nothing to keep, so the system temp folder.
    if (opt.data_dir.empty()) {
        std::error_code ec;
        const auto temp = std::filesystem::temp_directory_path(ec);
        opt.data_dir = ec ? std::string("userdata") : (temp / "nova3").string();
    }
    // the windows (launcher and game) take the executable's icon (resource 1)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    {
        std::error_code ec;
        if (opt.user_dir.empty()) {
            opt.user_dir = user_folder();
            // other game windows: their own saves and settings ("N.O.V.A. 3 (Player 2)"), starting
            // from player 1's settings the first time
            if (multi::slot() > 1) {
                const std::filesystem::path first = opt.user_dir;
                opt.user_dir += " (Player " + std::to_string(multi::slot()) + ")";
                std::filesystem::create_directories(opt.user_dir, ec);
                const auto ini = std::filesystem::path(opt.user_dir) / "nova3.ini";
                if (!std::filesystem::exists(ini, ec) && std::filesystem::exists(first / "nova3.ini", ec))
                    std::filesystem::copy_file(first / "nova3.ini", ini, ec);
            }
        }
        std::filesystem::create_directories(opt.user_dir, ec);
        if (ec) rt::fatal("cannot create the save folder %s: %s", opt.user_dir.c_str(), ec.message().c_str());
        if (opt.config.empty()) opt.config = (std::filesystem::path(opt.user_dir) / "nova3.ini").string();
        rt::log("save folder: %s", opt.user_dir.c_str());
        adopt_old_files(opt);
    }
    opt.cfg = settings::load(opt.config);
    if (opt.scripted && !opt.play) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    // tests stay silent (NOVA3_REAL_AUDIO=1 keeps the real device, e.g. for timing checks at volume 0)
    if (((opt.scripted && !opt.play) || opt.headless) && !std::getenv("NOVA3_REAL_AUDIO")) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    // controller events reach a window without focus too (several game windows, one controller
    // each: host/input.cpp still ignores the pad unfocused when this is the only window)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_AUDIO | (opt.headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)))
        rt::fatal("SDL_Init: %s", SDL_GetError());
    if (!opt.launcher_shot.empty()) {
        launcher::screenshot(opt.cfg, opt.config, opt.launcher_shot.c_str());
        rt::os::exit_process(0);
    }
    // The launcher, unless -silent, headless or a scripted test run.
    if (!opt.silent && !opt.scripted && !opt.headless && !launcher::run(opt.cfg, opt.config)) rt::os::exit_process(0);
    // [launcher] windows = N: split screen. This first window opens the shared window, then the
    // other N - 1 players, each its own process with a view inside it.
    if (opt.split == 0 && opt.cfg.windows > 1 && multi::slot() == 1 && !opt.scripted && !opt.headless) {
        opt.split = opt.cfg.windows;
        opt.split_display = opt.cfg.display;
        opt.pads = opt.cfg.split_pads;
        opt.names = opt.cfg.split_names;
        opt.screens = opt.cfg.split_screens;
        opt.split_fps = opt.cfg.split_fps;
        opt.shared_audio = opt.cfg.split_audio;
        opt.own_audio = opt.cfg.split_own_audio;
        split_layout(opt);
        if (opt.shared_count > 0) create_container(opt);  // everyone in a window of their own: none
        open_other_windows(opt);
    }
    if (opt.split > 1) {
        split_layout(opt);
        // the players in the shared window at the same frame rate: the shared limit replaces
        // their own; a player with a window of their own keeps their own ([video] max_fps)
        if (opt.own_monitor >= 0) {
            if (opt.own_fps >= 0) opt.cfg.max_fps = opt.own_fps;  // the launcher's Frame limit (player 1's)
            rt::log("own window: on monitor %d, frame limit %d fps (its own, not the shared one)",
                    opt.own_monitor + 1, opt.cfg.max_fps);
        } else {
            if (opt.split_fps > 0) opt.cfg.max_fps = opt.split_fps;
            rt::log("split screen: frame limit %d fps for every player in the shared window", opt.cfg.max_fps);
        }
    }
    if (opt.split > 1) input::set_split_pads(multi::split_list(opt.pads), multi::slot());
    // split screen: players 2-8 play under the names set in player 1's launcher
    if (opt.split > 1 && multi::slot() > 1) {
        const std::vector<std::string> names = multi::split_list(opt.names);
        if (size_t(multi::slot()) <= names.size() && !names[size_t(multi::slot() - 1)].empty())
            opt.cfg.nickname = names[size_t(multi::slot() - 1)];
        rt::log("name: %s", opt.cfg.nickname.empty() ? "(the game's default)" : opt.cfg.nickname.c_str());
    }
#ifdef _WIN32
    if (opt.cfg.console && g_log_redirected) open_log(true);  // switch the log from nova3.log to a console
#endif
    // Tests run windowed at --size (default 1280x720) so screenshots and tap coordinates hold.
    if (!opt.size_given && (!opt.scripted || opt.play)) opt.width = opt.cfg.width, opt.height = opt.cfg.height;
    input::configure(opt.cfg, opt.config);
    rt::net_set_peers(opt.cfg.peers);
    // test runs leave the player's profile alone (NOVA3_DISCORD_TEST=1 connects anyway)
    discord::start(opt.cfg, !opt.scripted || std::getenv("NOVA3_DISCORD_TEST"));
    input::set_scripted(opt.scripted && !opt.play);
    audio::set_volume(opt.cfg.volume / 100.0f);
    // split screen: the shared window's audio output, or this own-screen player's
    if (opt.split > 1) {
        std::string device = opt.shared_audio;
        if (opt.own_monitor >= 0) {
            const std::vector<std::string> own = multi::split_list(opt.own_audio);
            device = size_t(multi::slot()) <= own.size() ? own[size_t(multi::slot() - 1)] : "";
        }
        if (device == "mute") {
            audio::set_volume(0);
            rt::log("audio: muted");
        } else {
            audio::set_output_device(device);
        }
    }
    if (!opt.headless) {
        create_window(opt);
        input::set_window(g_window);
    }
    static const char *const kModes[] = {"windowed", "borderless", "fullscreen"};
    static const char *const kVSync[] = {"off", "on", "adaptive"};
    rt::log("video: %dx%d %s, vsync %s, max fps %d, anisotropy %dx", opt.width, opt.height,
            kModes[opt.scripted || opt.size_given ? 0 : int(opt.cfg.mode)], kVSync[int(opt.cfg.vsync)], opt.cfg.max_fps,
            opt.cfg.anisotropy);
    // Recompiled code nests one host frame per guest call: give the game a big stack.
    if (!rt::os::start_thread(game_thread, &boot, size_t(512) << 20)) rt::fatal("cannot start the game thread");
    // The main thread owns the window: handle events until the game thread ends or the
    // window is closed.
    void *view = g_embedded ? window_hwnd(g_window) : nullptr;
    if (view) input::set_view(view);
    void *container = (void *)(uintptr_t)opt.container;
    // split screen: one player may have the keyboard and mouse ([launcher] pads = kbm): then
    // that view always keeps them and the others never take them; otherwise they belong to the
    // view clicked last (player 1's at the start)
    int kbm_player = 0;
    if (opt.split > 1) {
        const std::vector<std::string> pads = multi::split_list(opt.pads);
        for (size_t i = 0; i < pads.size(); i++)
            if (pads[i] == "kbm") kbm_player = int(i) + 1;
    }
    const bool my_kbm = kbm_player == multi::slot(), kbm_elsewhere = kbm_player && !my_kbm;
    if (view && (kbm_player ? my_kbm : multi::slot() == 1)) multi::take_focus(view);
    if (view && kbm_player) rt::log("keyboard and mouse: %s", my_kbm ? "this player's" : "another player's");
    // a window of its own, with another player on the keyboard and mouse: only its controller
    if (!view && kbm_elsewhere) input::set_external_focus(false);
    while (!boot.done) {
        // the other players of a split close with the first one (and its window)
        if (opt.split > 1 && multi::slot() > 1 &&
            (!multi::slot_alive(1) || (opt.container && !multi::window_alive((void *)(uintptr_t)opt.container)))) {
            rt::log("player 1's window closed: closing this one too");
            rt::os::exit_process(0);
        }
        if (view) {
            // the keyboard-and-mouse player takes the focus back whenever the shared window is
            // active (after an alt-tab, or a click in another view)
            if (my_kbm && multi::is_foreground(container) && !multi::has_focus(view)) multi::take_focus(view);
            input::set_external_focus(!kbm_elsewhere && multi::has_focus(view));
        }
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        do {
            if (e.type == SDL_EVENT_QUIT) rt::os::exit_process(0);
            // the split screen's window: closing it ends every player
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ((g_container && e.window.windowID == SDL_GetWindowID(g_container)) ||
                 e.window.windowID == SDL_GetWindowID(g_window)))
                rt::os::exit_process(0);  // (an own window of player 2-8: only that player)
            if (view && !kbm_elsewhere && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                multi::take_focus(view);
                input::set_external_focus(true);
            }
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && e.window.windowID == SDL_GetWindowID(g_window))
                note_window_size(e.window.data1, e.window.data2);
            input::handle_event(e);
        } while (SDL_PollEvent(&e));
    }
    rt::os::exit_process(boot.result);  // guest worker threads may still be running
}
