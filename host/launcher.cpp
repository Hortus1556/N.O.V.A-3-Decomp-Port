#include "launcher.h"

#include "runtime.h"

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
bool g_hd_textures_found = false;  // launcher::set_hd_textures_found
}  // namespace

#ifdef _WIN32
namespace {

// Windows Firewall. Local Wi-Fi games from another computer (on the LAN, or a virtual LAN
// such as ZeroTier) reach the game as incoming UDP (the host listens on port 7892). Windows
// blocks incoming traffic for programs without a rule, silently when its notifications are
// off; two copies on one PC are not affected. The launcher adds an allow rule for this
// nova3.exe (netsh, after the administrator prompt).
std::wstring exe_path() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::wstring(buf, n);
}
std::wstring firewall_rule_name() { return L"N.O.V.A. 3 (" + exe_path() + L")"; }

bool firewall_rule_present() {
    std::wstring cmd = L"netsh advfirewall firewall show rule name=\"" + firewall_rule_name() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;  // 1: no rule of that name
}

bool add_firewall_rule() {
    const std::wstring args = L"advfirewall firewall add rule name=\"" + firewall_rule_name() +
                              L"\" dir=in action=allow enable=yes profile=any program=\"" + exe_path() + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";  // the administrator prompt
    sei.lpFile = L"netsh.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

}  // namespace
#endif

namespace launcher {
namespace {

using settings::DisplayMode;
using settings::Settings;

struct Display {
    SDL_DisplayID id;
    std::string name;
    int desktop_w, desktop_h;
    float desktop_hz;
    std::vector<std::pair<int, int>> sizes;  // fullscreen resolutions, largest first
    std::vector<SDL_DisplayMode> modes;
};

std::vector<Display> list_displays() {
    std::vector<Display> out;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    for (int i = 0; i < n; i++) {
        Display d{ids[i], SDL_GetDisplayName(ids[i]) ? SDL_GetDisplayName(ids[i]) : "Display", 0, 0, 0, {}, {}};
        if (const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(ids[i])) {
            d.desktop_w = dm->w;
            d.desktop_h = dm->h;
            d.desktop_hz = dm->refresh_rate;
        }
        int m = 0;
        if (SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(ids[i], &m)) {
            for (int k = 0; k < m; k++) {
                d.modes.push_back(*modes[k]);
                std::pair<int, int> wh{modes[k]->w, modes[k]->h};
                if (std::find(d.sizes.begin(), d.sizes.end(), wh) == d.sizes.end()) d.sizes.push_back(wh);
            }
            SDL_free(modes);
        }
        if (d.sizes.empty()) d.sizes.push_back({d.desktop_w, d.desktop_h});
        out.push_back(std::move(d));
    }
    SDL_free(ids);
    return out;
}

// Window sizes offered in windowed mode: common 16:9 sizes and the display's modes that fit.
std::vector<std::pair<int, int>> window_sizes(const Display &d) {
    std::vector<std::pair<int, int>> v = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (auto wh : d.sizes) v.push_back(wh);
    std::vector<std::pair<int, int>> out;
    for (auto wh : v)
        if (wh.first <= d.desktop_w && wh.second <= d.desktop_h && std::find(out.begin(), out.end(), wh) == out.end())
            out.push_back(wh);
    std::sort(out.begin(), out.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
    if (out.empty()) out.push_back({1280, 720});
    return out;
}

std::string size_label(std::pair<int, int> wh) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d x %d", wh.first, wh.second);
    return buf;
}

// Label + widget rows in a two-column table.
void row(const char *label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
bool begin_rows(const char *id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

template <class T>
bool combo(const char *id, T &value, const std::vector<std::pair<T, std::string>> &items) {
    std::string current = items.empty() ? "" : items.front().second;
    for (auto &[v, label] : items)
        if (v == value) current = label;
    bool changed = false;
    if (ImGui::BeginCombo(id, current.c_str())) {
        for (auto &[v, label] : items) {
            bool selected = v == value;
            if (ImGui::Selectable(label.c_str(), selected)) {
                value = v;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.PopupRounding = 4;
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
    ImVec4 *c = st.Colors;
    const ImVec4 accent(0.16f, 0.62f, 0.90f, 1.00f), accent_hi(0.30f, 0.72f, 0.98f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.20f, 0.28f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.28f, 0.40f, 1.00f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.24f, 0.34f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.30f, 0.44f, 1.00f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.30f, 0.40f, 1.00f);
    c[ImGuiCol_Text] = ImVec4(0.88f, 0.93f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.58f, 0.66f, 1.00f);
}

void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    for (const char *path : {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/System/Library/Fonts/SFNS.ttf"}) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && io.Fonts->AddFontFromFileTTF(path, 17.0f)) return;
    }
    io.Fonts->AddFontDefault();
}

const char *g_shot;  // --launcher-shot: PNG path; render a few frames, save, quit

}  // namespace

void set_hd_textures_found(bool found) { g_hd_textures_found = found; }

void screenshot(Settings cfg, const std::string &config_path, const char *png_path) {
    g_shot = png_path;
    run(cfg, config_path);
    g_shot = nullptr;
}

bool run(Settings &cfg, const std::string &config_path) {
    std::vector<Display> displays = list_displays();
    if (displays.empty()) return true;  // nothing to choose from: play with the saved settings
    cfg.display = std::clamp(cfg.display, 0, int(displays.size()) - 1);

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | (g_shot ? SDL_WINDOW_NOT_FOCUSABLE : 0);
    int win_h = int(860 * scale);  // the settings scroll, so small screens just get a shorter window
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) win_h = std::min(win_h, usable.h * 9 / 10);
    if (!SDL_CreateWindowAndRenderer("N.O.V.A. 3", int(640 * scale), win_h, flags, &window, &renderer)) {
        rt::log("launcher: %s; starting the game directly", SDL_GetError());
        return true;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    apply_style(scale);
    load_font();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // Frame limit presets (0 = off); a value outside the list shows as "Custom".
    const std::vector<int> fps_presets = {0, 30, 60, 120, 144, 165, 240, 360};
    bool custom_fps = std::find(fps_presets.begin(), fps_presets.end(), cfg.max_fps) == fps_presets.end();
    int result = -1;  // -1 running, 0 quit, 1 play
    int frames = 0;

    while (result < 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) result = 0;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(20);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.9f);
        ImGui::TextColored(ImVec4(0.30f, 0.72f, 0.98f, 1), "N.O.V.A. 3");
        ImGui::PopFont();
        ImGui::TextDisabled("Near Orbit Vanguard Alliance - native port");
        ImGui::Spacing();

        const Display &disp = displays[size_t(cfg.display)];
        const float footer_h = ImGui::GetFrameHeight() * 1.5f + ImGui::GetStyle().ItemSpacing.y * 2 + 1;
        ImGui::BeginChild("settings", ImVec2(0, -footer_h), ImGuiChildFlags_None);

        ImGui::SeparatorText("Display");
        if (begin_rows("display")) {
            if (displays.size() > 1) {
                row("Monitor");
                std::vector<std::pair<int, std::string>> items;
                for (size_t i = 0; i < displays.size(); i++)
                    items.push_back({int(i), std::to_string(i + 1) + ": " + displays[i].name});
                combo("##monitor", cfg.display, items);
            }
            row("Mode");
            combo<DisplayMode>("##mode", cfg.mode, {{DisplayMode::Windowed, "Windowed"},
                                                    {DisplayMode::Borderless, "Borderless fullscreen"},
                                                    {DisplayMode::Fullscreen, "Exclusive fullscreen"}});
            row("Resolution");
            if (cfg.mode == DisplayMode::Borderless) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s (desktop)", size_label({disp.desktop_w, disp.desktop_h}).c_str());
            } else {
                auto sizes = cfg.mode == DisplayMode::Fullscreen ? disp.sizes : window_sizes(disp);
                std::pair<int, int> cur{cfg.width, cfg.height};
                std::vector<std::pair<std::pair<int, int>, std::string>> items;
                for (auto wh : sizes) items.push_back({wh, size_label(wh)});
                if (std::find(sizes.begin(), sizes.end(), cur) == sizes.end()) items.insert(items.begin(), {cur, size_label(cur) + " (custom)"});
                if (combo("##res", cur, items)) {
                    cfg.width = cur.first;
                    cfg.height = cur.second;
                }
            }
            if (cfg.mode == DisplayMode::Fullscreen) {
                row("Refresh rate");
                std::vector<std::pair<int, std::string>> items = {{0, "Highest"}};
                for (const SDL_DisplayMode &m : disp.modes)
                    if (m.w == cfg.width && m.h == cfg.height) {
                        int hz = int(m.refresh_rate + 0.5f);
                        bool dup = false;
                        for (auto &it : items) dup |= it.first == hz;
                        if (!dup && hz > 0) items.push_back({hz, std::to_string(hz) + " Hz"});
                    }
                combo("##hz", cfg.refresh, items);
            }
            row("VSync");
            combo<Settings::VSync>("##vsync", cfg.vsync, {{Settings::VSync::Off, "Off"},
                                                          {Settings::VSync::On, "On"},
                                                          {Settings::VSync::Adaptive, "Adaptive"}});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Adaptive: synced when the game keeps up, tears instead of stuttering when it doesn't.");
            row("Frame limit");
            {
                int choice = custom_fps ? -1 : cfg.max_fps;
                std::vector<std::pair<int, std::string>> items = {{0, "Unlimited"}};
                for (int f : fps_presets)
                    if (f) items.push_back({f, std::to_string(f) + " fps"});
                items.push_back({-1, "Custom"});
                if (custom_fps || cfg.vsync != Settings::VSync::Off) ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                if (combo("##fps", choice, items)) {
                    custom_fps = choice == -1;
                    if (!custom_fps) cfg.max_fps = choice;
                    else if (cfg.max_fps <= 0) cfg.max_fps = 100;
                }
                if (custom_fps) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt("##fpsval", &cfg.max_fps, 5, 30)) cfg.max_fps = std::clamp(cfg.max_fps, 10, 1000);
                } else if (cfg.vsync != Settings::VSync::Off) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("with VSync");
                }
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Graphics");
        if (begin_rows("graphics")) {
            row("Texture filtering");
            combo("##aniso", cfg.anisotropy, std::vector<std::pair<int, std::string>>{
                {1, "Trilinear"}, {2, "Anisotropic 2x"}, {4, "Anisotropic 4x"}, {8, "Anisotropic 8x"}, {16, "Anisotropic 16x"}});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sharper textures at grazing angles (floors, walls).");
            row("HD textures");
            ImGui::BeginDisabled(!g_hd_textures_found);
            ImGui::Checkbox("##hdtex", &cfg.hd_textures);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(g_hd_textures_found ? "upscaled textures (hd_textures.pak); off: the originals"
                                                    : "hd_textures.pak not found (tools/texpack.py makes it)");
            row("Field of view");
            {
                int fov = cfg.fov ? cfg.fov : 58;  // the game's own view is about 58 degrees (16:9)
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Default").x -
                                        ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
                if (ImGui::SliderInt("##fov", &fov, 50, 120, cfg.fov ? "%d°" : "%d° (game default)")) cfg.fov = fov;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Horizontal field of view at 16:9. Aiming still zooms in from it.");
                ImGui::SameLine();
                if (ImGui::Button("Default")) cfg.fov = 0;
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Audio");
        if (begin_rows("audio")) {
            row("Master volume");
            ImGui::SliderInt("##vol", &cfg.volume, 0, 100, cfg.volume ? "%d%%" : "muted");
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Controls");
        if (begin_rows("controls")) {
            row("Sensitivity");
            ImGui::SliderFloat("##sens", &cfg.sensitivity, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            row("Aim sensitivity");
            ImGui::SliderFloat("##aimsens", &cfg.aim_sensitivity, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            row("Invert Y");
            ImGui::Checkbox("##inv", &cfg.invert_y);
            row("Gamepad");
            ImGui::Checkbox("##pad", &cfg.gamepad);
            {
                int n = 0;
                SDL_JoystickID *pads = SDL_GetGamepads(&n);
                ImGui::SameLine();
                if (n > 0) ImGui::TextDisabled("%s", SDL_GetGamepadNameForID(pads[0]) ? SDL_GetGamepadNameForID(pads[0]) : "controller connected");
                else ImGui::TextDisabled("no controller connected");
                SDL_free(pads);
            }
            if (cfg.gamepad) {
                row("Stick dead zone");
                ImGui::SliderFloat("##dz", &cfg.stick_deadzone, 0.05f, 0.5f, "%.2f");
                row("Stick look speed");
                ImGui::SliderFloat("##look", &cfg.look_speed, 60.0f, 480.0f, "%.0f deg/s");
                row("Aim assist");
                ImGui::Checkbox("##assist", &cfg.aim_assist);
                row("Auto-leveling");
                ImGui::Checkbox("##level", &cfg.auto_leveling);
                row("Rumble");
                ImGui::Checkbox("##rumble", &cfg.rumble);
                if (cfg.rumble) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    int strength = int(std::lround(cfg.rumble_strength * 100));
                    if (ImGui::SliderInt("##rumblestr", &strength, 0, 100, "strength %d%%")) cfg.rumble_strength = strength / 100.0f;
                }
            }
            row("Toggle sprint");
            ImGui::Checkbox("##togglesprint", &cfg.toggle_sprint);
            ImGui::SameLine();
            ImGui::TextDisabled(cfg.toggle_sprint ? "press to run, again to walk" : "hold to run");
            row("Key bindings");
            if (ImGui::Button("Edit in nova3.ini...")) {
                std::error_code ec;
                std::string url = "file:///" + std::filesystem::absolute(config_path, ec).generic_string();
                SDL_OpenURL(url.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(or in the game: Options > Controls)");
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Multiplayer");
        if (begin_rows("multiplayer")) {
            row("Nickname");
            static char nick[17];
            static bool nick_loaded = false;
            if (!nick_loaded) {
                std::snprintf(nick, sizeof nick, "%s", cfg.nickname.c_str());
                nick_loaded = true;
            }
            // letters, digits and simple punctuation: the game's fonts and the ini line
            auto filter = [](ImGuiInputTextCallbackData *d) {
                const ImWchar c = d->EventChar;
                return (c >= 32 && c < 127 && c != ';' && c != '[' && c != ']') ? 0 : 1;
            };
            ImGui::SetNextItemWidth(260);
            if (ImGui::InputTextWithHint("##nick", "the game's default", nick, sizeof nick,
                                         ImGuiInputTextFlags_CallbackCharFilter, filter))
                cfg.nickname = nick;
            ImGui::SameLine();
            ImGui::TextDisabled("shown to other players on local Wi-Fi");
            row("Players' IPs");
            static char peers[128];
            static bool peers_loaded = false;
            if (!peers_loaded) {
                std::snprintf(peers, sizeof peers, "%s", cfg.peers.c_str());
                peers_loaded = true;
            }
            auto ip_filter = [](ImGuiInputTextCallbackData *d) {
                const ImWchar c = d->EventChar;
                return ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == ' ') ? 0 : 1;
            };
            ImGui::SetNextItemWidth(260);
            if (ImGui::InputTextWithHint("##peers", "optional, e.g. 10.147.17.5", peers, sizeof peers,
                                         ImGuiInputTextFlags_CallbackCharFilter, ip_filter))
                cfg.peers = peers;
            ImGui::SameLine();
            ImGui::TextDisabled("if games are not found (ZeroTier, VPN)");
#ifdef _WIN32
            row("Other computers");
            static int firewall = -1;  // -1 unknown, 0 no rule, 1 allowed
            if (firewall < 0) firewall = firewall_rule_present() ? 1 : 0;
            if (firewall == 1) {
                ImGui::TextUnformatted("Allowed through Windows Firewall");
            } else {
                if (ImGui::Button("Allow through Windows Firewall...")) {
                    add_firewall_rule();
                    firewall = firewall_rule_present() ? 1 : 0;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("needed to play with other PCs (LAN, ZeroTier)");
            }
#endif
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Discord");
        if (begin_rows("discord")) {
            row("Rich Presence");
            ImGui::Checkbox("##discord", &cfg.discord);
            ImGui::SameLine();
            ImGui::TextDisabled("show what you are playing on your Discord profile");
            row("Application ID");
            static char app_id[32];
            static bool app_id_loaded = false;
            if (!app_id_loaded) {
                std::snprintf(app_id, sizeof app_id, "%s", cfg.discord_app_id.c_str());
                app_id_loaded = true;
            }
            ImGui::BeginDisabled(!cfg.discord);
            ImGui::SetNextItemWidth(260);
            if (ImGui::InputTextWithHint("##appid", "from discord.com/developers", app_id, sizeof app_id,
                                         ImGuiInputTextFlags_CharsDecimal))
                cfg.discord_app_id = app_id;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("its name is shown as the game");
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Other");
        if (begin_rows("other")) {
            row("Saves and settings");
            if (ImGui::Button("Open folder...")) {  // nova3.ini sits in the user folder, next to save/
                std::error_code ec;
                auto dir = std::filesystem::absolute(config_path, ec).parent_path();
                SDL_OpenURL(("file:///" + dir.generic_string()).c_str());
            }
#ifdef _WIN32
            row("Log console");
            ImGui::Checkbox("##console", &cfg.console);
            ImGui::SameLine();
            ImGui::TextDisabled(cfg.console ? "log shown in a console window" : "log written to nova3.log");
#endif
            ImGui::EndTable();
        }

        ImGui::EndChild();

        // Footer: tip on the left, Quit / Play on the right
        ImGui::Separator();
        float bh = ImGui::GetFrameHeight() * 1.5f, bw = ImGui::GetFontSize() * 7;
        float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (bh - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextDisabled("Tip: nova3.exe -silent skips this window.");
        const bool hotkeys = !ImGui::GetIO().WantTextInput;  // Enter = Play, Esc = Quit (not while typing)
        const float x_quit = ImGui::GetContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x_quit, y));
        if (ImGui::Button("Quit", ImVec2(bw, bh)) || (hotkeys && ImGui::IsKeyPressed(ImGuiKey_Escape))) result = 0;
        ImGui::SetCursorPos(ImVec2(x_quit + bw + ImGui::GetStyle().ItemSpacing.x, y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.50f, 0.80f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.60f, 0.92f, 1));
        if (ImGui::Button("Play", ImVec2(bw, bh)) || (hotkeys && ImGui::IsKeyPressed(ImGuiKey_Enter))) result = 1;
        ImGui::PopStyleColor(2);
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 15, 20, 28, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (g_shot && ++frames == 5) {
            if (SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SavePNG(surface, g_shot);
                SDL_DestroySurface(surface);
            }
            result = 0;
        }
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (result != 1) return false;

    // Key bindings may have been edited in nova3.ini meanwhile: keep the file's version.
    Settings on_disk = settings::load(config_path);
    cfg.bindings = on_disk.bindings;
    cfg.binding_text = on_disk.binding_text;
    if (!settings::save(config_path, cfg)) rt::log("launcher: could not save %s", config_path.c_str());
    return true;
}

}  // namespace launcher
