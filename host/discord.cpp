// Discord Rich Presence through the Discord client's local IPC: a named pipe on Windows
// (\\.\pipe\discord-ipc-0 .. 9), a Unix socket elsewhere ($XDG_RUNTIME_DIR, $TMPDIR, ... /
// discord-ipc-0 .. 9). Every message is a frame: opcode (u32), length (u32), JSON. A
// handshake (opcode 0) names the application; SET_ACTIVITY commands (opcode 1) then set what
// the profile shows, until the connection closes. No Discord library is needed.
//
// The game thread reads the state about once a second (frame()); a background thread
// connects (again every 10 s while Discord is not running), sends the activity when it
// changes, and reads and drops Discord's replies.
#include "discord.h"
#include "hooks.h"

#include "os.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace discord {
namespace {

struct Activity {
    std::string details, state;
    int64_t start = 0;  // Unix seconds: the elapsed time shown
    bool operator==(const Activity &o) const { return details == o.details && state == o.state && start == o.start; }
};

std::mutex g_lock;
Activity g_wanted;       // guarded by g_lock
uint64_t g_version = 0;  // bumped with each change of g_wanted
std::string g_app_id;
std::atomic<bool> g_started{false};

std::string json_string(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += char(c);
    }
    return out + "\"";
}

// ---- the connection -----------------------------------------------------------------------

class Pipe {
public:
    bool open() {
        for (int i = 0; i < 10 && !connected(); i++) open_one(i);
        return connected();
    }
    bool connected() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }
    void close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }
    bool send(uint32_t op, const std::string &json) {
        std::string frame(8, '\0');
        const uint32_t len = uint32_t(json.size());
        std::memcpy(&frame[0], &op, 4);  // little-endian on every host this builds for
        std::memcpy(&frame[4], &len, 4);
        frame += json;
        return write_all(frame.data(), frame.size());
    }
    // One reply (opcode, JSON) within `ms`, or false.
    bool receive(uint32_t &op, std::string &json, int ms) {
        char header[8];
        if (!read_all(header, 8, ms)) return false;
        uint32_t len;
        std::memcpy(&op, header, 4);
        std::memcpy(&len, header + 4, 4);
        if (len > 1 << 20) return false;
        json.assign(len, '\0');
        return len == 0 || read_all(&json[0], len, ms);
    }
    // Drops the replies waiting; false if the connection is gone.
    bool drain() {
        char buf[4096];
        for (;;) {
            const long n = available();
            if (n < 0) return false;
            if (n == 0) return true;
            if (!read_some(buf, std::min<long>(n, sizeof buf))) return false;
        }
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
    void open_one(int i) {
        const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    bool write_all(const char *p, size_t n) {
        DWORD done = 0;
        return WriteFile(h_, p, DWORD(n), &done, nullptr) && done == n;
    }
    long available() {
        DWORD n = 0;
        return PeekNamedPipe(h_, nullptr, 0, nullptr, &n, nullptr) ? long(n) : -1;
    }
    bool read_some(char *p, long n) {
        DWORD done = 0;
        return ReadFile(h_, p, DWORD(n), &done, nullptr) && done > 0;
    }
#else
    int fd_ = -1;
    void open_one(int i) {
        for (const char *var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
            const char *dir = std::getenv(var);
            if (try_path(std::string(dir ? dir : "") + "/discord-ipc-" + std::to_string(i))) return;
        }
        try_path("/tmp/discord-ipc-" + std::to_string(i));
    }
    bool try_path(const std::string &path) {
        if (path.size() < 2 || path[0] != '/') return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (path.size() >= sizeof a.sun_path) return false;
        std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ >= 0 && connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) return true;
        close();
        return false;
    }
    bool write_all(const char *p, size_t n) {
        while (n) {
            const ssize_t r = ::send(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r, n -= size_t(r);
        }
        return true;
    }
    long available() {
        char c;
        const ssize_t r = recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r > 0) return 4096;
        if (r == 0) return -1;  // closed
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    bool read_some(char *p, long n) { return recv(fd_, p, size_t(n), MSG_DONTWAIT) > 0; }
#endif
    bool read_all(char *p, size_t n, int ms) {
        const int64_t deadline = rt::os::monotonic_ns() + int64_t(ms) * 1000000;
        while (n) {
            const long avail = available();
            if (avail < 0) return false;
            if (avail == 0) {
                if (rt::os::monotonic_ns() > deadline) return false;
                rt::os::sleep_ns(5'000'000);
                continue;
            }
            const long want = std::min<long>(avail, long(n));
#ifdef _WIN32
            DWORD done = 0;
            if (!ReadFile(h_, p, DWORD(want), &done, nullptr) || !done) return false;
            p += done, n -= done;
#else
            const ssize_t r = recv(fd_, p, size_t(want), 0);
            if (r <= 0) return false;
            p += r, n -= size_t(r);
#endif
        }
        return true;
    }
};

std::string activity_command(const Activity &a, uint64_t nonce) {
    std::string act = "{\"details\":" + json_string(a.details);
    if (!a.state.empty()) act += ",\"state\":" + json_string(a.state);
    if (a.start) act += ",\"timestamps\":{\"start\":" + std::to_string(a.start) + "}";
    act += "}";
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(rt::os::process_id()) +
           ",\"activity\":" + act + "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

void run() {
    Pipe pipe;
    int64_t next_try = 0;
    uint64_t sent = 0, nonce = 0;
    bool told_missing = false;
    for (;;) {
        rt::os::sleep_ns(250'000'000);
        if (!pipe.connected()) {
            if (rt::os::monotonic_ns() < next_try) continue;
            next_try = rt::os::monotonic_ns() + 10'000'000'000LL;
            if (!pipe.open()) {
                if (!told_missing) rt::log("discord: not running (trying again every 10 s)");
                told_missing = true;
                continue;
            }
            uint32_t op = 0;
            std::string reply;
            if (!pipe.send(0, "{\"v\":1,\"client_id\":" + json_string(g_app_id) + "}") || !pipe.receive(op, reply, 3000) ||
                reply.find("\"READY\"") == std::string::npos) {
                rt::log("discord: handshake refused (check [discord] app_id): %s", reply.substr(0, 200).c_str());
                pipe.close();
                next_try = rt::os::monotonic_ns() + 60'000'000'000LL;  // a wrong ID stays wrong
                continue;
            }
            rt::log("discord: connected");
            told_missing = false;
            sent = 0;
        }
        Activity wanted;
        uint64_t version;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            wanted = g_wanted;
            version = g_version;
        }
        if (version != sent && !wanted.details.empty()) {
            if (!pipe.send(1, activity_command(wanted, ++nonce))) {
                rt::log("discord: connection lost");
                pipe.close();
                continue;
            }
            sent = version;
        }
        if (!pipe.drain()) {
            rt::log("discord: connection lost");
            pipe.close();
        }
    }
}

// ---- the game's state ---------------------------------------------------------------------

// A string of the menus' table (MAIN; Application +0x18758), whatever table is current: in a
// level the level's own is (see host/prompts.cpp). Line breaks ('|', '\n') become spaces.
std::string main_string(uint32_t app, int id) {
    const uint32_t strings = app ? LD32(app + 0x18758) : 0;
    if (!strings || id < 0 || uint32_t(id) >= (LD32(strings + 0x18) - LD32(strings + 0x14)) / 4) return "";
    std::string s = rt::str(LD32(strings) + LD32(LD32(strings + 8) + 4 * uint32_t(id)));
    for (char &c : s)
        if (c == '|' || c == '\n') c = ' ';
    return s;
}

// The game's own mode names (CMenuManager::GetMPGameModeStrIdFromId), by GameMpManager +0x3c00
const int kModeNames[] = {0x27d /* Free-for-All */, 0x27e /* Team Deathmatch */, 0x280 /* Freeze Tag */,
                          0x27f /* Capture the Flag */, 0x283 /* Capture the Point */, 0x281 /* InstaGib */,
                          0x284 /* Team InstaGib */, 0x282 /* Co-op */};

Activity read_state(Cpu *cpu) {
    struct Fns {
        static uint32_t f(const char *n) { return rt::export_address(n); }
        uint32_t app = f("_ZN11Application11GetInstanceEv"), level = f("_ZN6CLevel8GetLevelEv");
        uint32_t levels = f("_ZN14CLevelsManager9SingletonE"), mp = f("_ZN13GameMpManager9SingletonE");
        uint32_t title = f("_ZNK14CLevelsManager16GetLevelTitleStrEi");
        uint32_t mp_title = f("_ZNK14CLevelsManager18GetMPLevelTitleStrEi");
    };
    static const Fns fns;
    const uint32_t app = rt::call_nested(cpu, fns.app, {});
    const bool in_level = rt::call_nested(cpu, fns.level, {}) != 0;
    const uint32_t mp = fns.mp ? LD32(fns.mp) : 0, levels = fns.levels ? LD32(fns.levels) : 0;
    const bool multiplayer = hooks::multiplayer();
    Activity a;
    if (multiplayer) {
        const uint32_t mode = LD32(mp + 0x3c00);
        const std::string mode_name = mode < 8 ? main_string(app, kModeNames[mode]) : "";
        a.details = mode_name.empty() ? "Local multiplayer" : "Local multiplayer: " + mode_name;
        if (in_level && levels)  // CLevelsManager +0x28: the match's map
            a.state = main_string(app, int32_t(rt::call_nested(cpu, fns.mp_title, {levels, LD32(levels + 0x28)})));
        else
            a.state = "In the lobby";
    } else if (in_level && levels && int32_t(LD32(levels + 0x18)) >= 0) {  // +0x18: the campaign level
        const int index = int32_t(LD32(levels + 0x18));
        a.details = "Campaign";
        const std::string title = main_string(app, int32_t(rt::call_nested(cpu, fns.title, {levels, uint32_t(index)})));
        a.state = "Chapter " + std::to_string(index + 1) + (title.empty() ? "" : ": " + title);
    } else {
        a.details = "In the menus";
    }
    return a;
}

}  // namespace

void start(const settings::Settings &s, bool connect) {
    if (connect && (!s.discord || s.discord_app_id.empty())) return;
    if (g_started.exchange(true)) return;
    g_app_id = s.discord_app_id;
    if (connect) std::thread(run).detach();
}

void frame(Cpu *cpu) {
    if (!g_started) return;
    static int64_t next = 0;
    const int64_t now = rt::os::monotonic_ns();
    if (now < next) return;
    next = now + 1'000'000'000;
    Activity a = read_state(cpu);
    std::lock_guard<std::mutex> lock(g_lock);
    if (a.details == g_wanted.details && a.state == g_wanted.state) return;
    // the elapsed time restarts with a new activity (menus, campaign, a match), not with the
    // next chapter or map of the same one
    a.start = a.details == g_wanted.details ? g_wanted.start : int64_t(std::time(nullptr));
    g_wanted = a;
    g_version++;
    rt::log("discord: %s%s%s", a.details.c_str(), a.state.empty() ? "" : " / ", a.state.c_str());
}

}  // namespace discord
