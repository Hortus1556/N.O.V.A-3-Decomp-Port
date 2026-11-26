#include "multi.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace multi {
namespace {

constexpr int kMaxSlots = 8;
int g_slot = 1;    // the slot (named mutex) this process holds
int g_player = 0;  // the player number player 1 gave it (--player), or 0: the slot's

#ifdef _WIN32
HANDLE g_slot_mutex;
HANDLE g_pad_mutex;

// Local\: this login session. A mutex that already exists belongs to another process.
HANDLE create_unique(const wchar_t *name) {
    HANDLE h = CreateMutexW(nullptr, FALSE, name);
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return nullptr;
    }
    return h;
}

void slot_name(int n, wchar_t *out, size_t len) { _snwprintf_s(out, len, _TRUNCATE, L"Local\\NOVA3_SLOT_%d", n); }
#endif

}  // namespace

int claim_slot(int preferred) {
#ifdef _WIN32
    if (g_slot_mutex) return g_slot;
    // the player's own slot first (the windows of a split screen), then the first free one
    for (int i = 0; i <= kMaxSlots; i++) {
        const int n = i == 0 ? preferred : i;
        if (n < 1 || n > kMaxSlots || (i > 0 && n == preferred)) continue;
        wchar_t name[64];
        slot_name(n, name, 64);
        if (HANDLE h = create_unique(name)) {
            g_slot_mutex = h;
            g_slot = n;
            return n;
        }
    }
#endif
    return g_slot;  // every slot taken (or not Windows): play as player 1
}

int slot() { return g_player ? g_player : g_slot; }
int held_slot() { return g_slot; }
void set_player(int player) { g_player = player >= 1 && player <= kMaxSlots ? player : 0; }

bool others_running() {
#ifdef _WIN32
    using clock = std::chrono::steady_clock;
    static std::atomic<int64_t> next_check{0};
    static std::atomic<bool> cached{false};
    const int64_t now = clock::now().time_since_epoch().count();
    if (now < next_check.load()) return cached;
    next_check = now + std::chrono::duration_cast<clock::duration>(std::chrono::seconds(1)).count();
    bool found = false;
    for (int n = 1; n <= kMaxSlots && !found; n++) {
        if (n == g_slot && g_slot_mutex) continue;
        wchar_t name[64];
        slot_name(n, name, 64);
        if (HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name)) {
            CloseHandle(h);
            found = true;
        }
    }
    cached = found;
    return found;
#else
    return false;
#endif
}

bool slot_alive(int n) {
#ifdef _WIN32
    if (n == g_slot && g_slot_mutex) return true;
    wchar_t name[64];
    slot_name(n, name, 64);
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (h) CloseHandle(h);
    return h != nullptr;
#else
    return n == g_slot;
#endif
}

void grid(int windows, int &cols, int &rows) {
    if (windows <= 3) cols = windows < 1 ? 1 : windows, rows = 1;
    else if (windows == 4) cols = 2, rows = 2;
    else if (windows <= 6) cols = 3, rows = 2;
    else cols = 4, rows = 2;
}

bool claim_pad(const std::string &key) {
#ifdef _WIN32
    release_pad();
    // FNV-1a of the key: device paths hold backslashes, which mutex names cannot
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE, L"Local\\NOVA3_PAD_%016llx", (unsigned long long)h);
    g_pad_mutex = create_unique(name);
    return g_pad_mutex != nullptr;
#else
    (void)key;
    return true;
#endif
}

void release_pad() {
#ifdef _WIN32
    if (g_pad_mutex) CloseHandle(g_pad_mutex);
    g_pad_mutex = nullptr;
#endif
}

}  // namespace multi

namespace multi {

std::vector<std::string> split_list(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t bar = s.find('|', start);
        if (bar == std::string::npos) bar = s.size();
        out.push_back(s.substr(start, bar - start));
        start = bar + 1;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::string join_list(const std::vector<std::string> &v) {
    size_t n = v.size();
    while (n > 0 && v[n - 1].empty()) n--;
    std::string out;
    for (size_t i = 0; i < n; i++) out += (i ? "|" : "") + v[i];
    return out;
}

void tile_rect(void *container, int windows, int slot, int &x, int &y, int &w, int &h) {
    int cw = 1280, ch = 720;
#ifdef _WIN32
    RECT rc;
    if (container && GetClientRect(HWND(container), &rc)) cw = rc.right - rc.left, ch = rc.bottom - rc.top;
#else
    (void)container;
#endif
    // The game lays its screens out for phones and tablets, 4:3 to 16:9: a taller view cuts off
    // the menus' sides (960x1080 loses the left-hand buttons), a wider one the top and bottom.
    // So each view keeps a shape in that range, centered in its cell (black bars around), and
    // the grid is the one giving the views the most area.
    auto fit = [](int cell_w, int cell_h, int &fw, int &fh) {
        fw = cell_w, fh = cell_h;
        if (fw * 3 > fh * 4) fw = fh * 16 / 9 < fw ? fh * 16 / 9 : fw;  // wider than 4:3: at most 16:9
        if (fw * 3 < fh * 4) fh = fw * 3 / 4;                          // taller than 4:3: cut to 4:3
    };
    if (windows < 1) windows = 1;
    int cols = 1, rows = windows;
    long long best = -1;
    for (int c = 1; c <= windows; c++) {
        const int r = (windows + c - 1) / c;
        int fw, fh;
        fit(cw / c, ch / r, fw, fh);
        if (1LL * fw * fh > best) best = 1LL * fw * fh, cols = c, rows = r;
    }
    const int tile = (slot - 1) % windows;
    const int c = tile % cols, r = tile / cols;
    const int cell_w = cw / cols, cell_h = ch / rows;
    // a last row with fewer players is centered
    const int in_row = r == rows - 1 ? windows - (rows - 1) * cols : cols;
    const int row_x = (cw - in_row * cell_w) / 2, grid_y = (ch - rows * cell_h) / 2;
    fit(cell_w, cell_h, w, h);
    x = row_x + c * cell_w + (cell_w - w) / 2;
    y = grid_y + r * cell_h + (cell_h - h) / 2;
}

bool embed(void *window, void *container, int x, int y, int w, int h) {
#ifdef _WIN32
    HWND hw = HWND(window), parent = HWND(container);
    if (!hw || !IsWindow(parent)) return false;
    SetWindowLongPtrW(hw, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetWindowLongPtrW(hw, GWL_EXSTYLE, WS_EX_NOPARENTNOTIFY);
    if (!SetParent(hw, parent)) return false;
    SetWindowPos(hw, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
#else
    (void)window, (void)container, (void)x, (void)y, (void)w, (void)h;
    return false;
#endif
}

bool has_focus(void *window) {
#ifdef _WIN32
    // the focus of the foreground thread: with the views' input queues attached (SetParent
    // across processes), that is the view the player last clicked
    GUITHREADINFO gi{sizeof gi};
    if (!GetGUIThreadInfo(0, &gi)) return false;
    return gi.hwndFocus == HWND(window);
#else
    (void)window;
    return true;
#endif
}

void take_focus(void *window) {
#ifdef _WIN32
    SetFocus(HWND(window));
#else
    (void)window;
#endif
}

bool is_foreground(void *container) {
#ifdef _WIN32
    return container && GetForegroundWindow() == HWND(container);
#else
    (void)container;
    return true;
#endif
}

#ifdef _WIN32
// the view's client area on the screen, and its center
bool view_rect(void *view, RECT &r, POINT &center) {
    if (!view || !GetClientRect(HWND(view), &r)) return false;
    MapWindowPoints(HWND(view), nullptr, reinterpret_cast<POINT *>(&r), 2);
    center = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    return r.right > r.left && r.bottom > r.top;
}
#endif

void lock_cursor(void *view) {
#ifdef _WIN32
    RECT r;
    POINT c;
    if (!view_rect(view, r, c)) return;
    ClipCursor(&r);
    SetCursorPos(c.x, c.y);
#else
    (void)view;
#endif
}

void unlock_cursor() {
#ifdef _WIN32
    ClipCursor(nullptr);
#endif
}

void recenter_cursor(void *view, int &dx, int &dy) {
    dx = dy = 0;
#ifdef _WIN32
    RECT r;
    POINT c, p;
    if (!view_rect(view, r, c) || !GetCursorPos(&p)) return;
    dx = p.x - c.x;
    dy = p.y - c.y;
    ClipCursor(&r);  // again: Windows drops the clip on some focus changes
    if (dx || dy) SetCursorPos(c.x, c.y);
#else
    (void)view;
#endif
}

bool window_alive(void *container) {
#ifdef _WIN32
    return container && IsWindow(HWND(container));
#else
    return container != nullptr;
#endif
}

}  // namespace multi
