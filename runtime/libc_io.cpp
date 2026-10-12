// Files: path sandbox, fd table, stat/dirs, stdio FILE objects, printf/scanf families.
//
// Guest paths are mapped into FsConfig::data_dir (an Android-like tree) except the OBB
// directory, which points at the user's OBB folder. /proc and /sys files the game reads
// are generated at startup (notably a /proc/cpuinfo without NEON so the C code paths
// are chosen instead of NEON assembly).
#if defined(__3DS__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // newlib's fopencookie (packed game files)
#endif
#include "os.h"
#include "tls.h"
#include "runtime.h"

#include <cstdlib>

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#ifndef __GAMECUBE__
#include <poll.h>
#endif
#include <unistd.h>
#endif
#ifdef __3DS__
#include <zlib.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string_view>
#include <unordered_set>
#include <chrono>
#include <vector>

namespace fs = std::filesystem;


// Host file descriptors: the CRT's POSIX-style calls on Windows, the real ones elsewhere.
namespace hio {
#ifdef _WIN32
using stat_t = struct _stat64;
inline int open(const char *p, int flags) { return ::_open(p, flags | _O_BINARY, _S_IREAD | _S_IWRITE); }
inline int read(int h, void *b, unsigned n) {
    if (!rt::os::working_set_on()) return ::_read(h, b, n);
    std::vector<uint8_t> t(n);  // the profile may have taken the guest buffer's access (os.h)
    const int r = ::_read(h, t.data(), n);
    if (r > 0) std::memcpy(b, t.data(), size_t(r));
    return r;
}
// Writes are counted per pipe (both ends share the count) for edge-triggered epoll: boost::asio
// registers its interrupter pipe EPOLLET and never drains it, so a level-triggered readable
// kept epoll_wait returning at once and its reactor thread spun a whole core.
inline std::mutex g_pipe_lock;
inline std::unordered_map<int, std::shared_ptr<uint64_t>> g_pipe_writes;  // handle -> its pipe's write count
inline int write(int h, const void *b, unsigned n) {
    std::vector<uint8_t> t;
    if (rt::os::working_set_on()) t.assign(static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n), b = t.data();
    const int r = ::_write(h, b, n);
    if (r > 0) {
        std::lock_guard<std::mutex> lock(g_pipe_lock);
        if (auto it = g_pipe_writes.find(h); it != g_pipe_writes.end()) ++*it->second;
    }
    return r;
}
inline int64_t lseek(int h, int64_t off, int whence) { return ::_lseeki64(h, off, whence); }
inline int close(int h) {
    {
        std::lock_guard<std::mutex> lock(g_pipe_lock);
        g_pipe_writes.erase(h);
    }
    return ::_close(h);
}
inline int dup(int h) { return ::_dup(h); }
inline int unlink(const char *p) { return ::_unlink(p); }
inline int stat(const char *p, stat_t *s) { return ::_stat64(p, s); }
inline int fstat(int h, stat_t *s) { return ::_fstat64(h, s); }
inline bool is_dir(const stat_t &s) { return (s.st_mode & _S_IFDIR) != 0; }
inline int pipe(int h[2]) {
    const int r = ::_pipe(h, 65536, _O_BINARY);
    if (r == 0) {
        auto count = std::make_shared<uint64_t>(0);
        std::lock_guard<std::mutex> lock(g_pipe_lock);
        g_pipe_writes[h[0]] = g_pipe_writes[h[1]] = count;
    }
    return r;
}
#define NOVA3_PIPE_EDGES 1
inline uint64_t pipe_writes(int h) {
    std::lock_guard<std::mutex> lock(g_pipe_lock);
    auto it = g_pipe_writes.find(h);
    return it != g_pipe_writes.end() ? *it->second : 0;
}
inline int fileno(FILE *f) { return ::_fileno(f); }
inline FILE *fdopen(int h, const char *mode) { return ::_fdopen(h, mode); }
inline void lock_file(FILE *f) { ::_lock_file(f); }
inline void unlock_file(FILE *f) { ::_unlock_file(f); }
constexpr int kRdOnly = _O_RDONLY, kWrOnly = _O_WRONLY, kRdWr = _O_RDWR, kCreat = _O_CREAT, kExcl = _O_EXCL,
              kTrunc = _O_TRUNC, kAppend = _O_APPEND;
inline bool is_pipe(int h) { return GetFileType(reinterpret_cast<HANDLE>(_get_osfhandle(h))) == FILE_TYPE_PIPE; }
// pipe with data to read (or broken: a read returns EOF immediately)
inline bool pipe_readable(int h) {
    DWORD avail = 0;
    if (!PeekNamedPipe(reinterpret_cast<HANDLE>(_get_osfhandle(h)), nullptr, 0, nullptr, &avail, nullptr)) return true;
    return avail > 0;
}
#elif defined(__SWITCH__) || defined(__3DS__) || defined(__GAMECUBE__)
// newlib on the Switch (and the 3DS, the GameCube) has no pipes (boost::asio's reactor wakes itself with one): pipes are
// emulated in-process, as handles from kPipeBase up (the two ends of pipe n: 2n and 2n+1).
using stat_t = struct ::stat;
struct Pipe {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<char> data;
    int readers = 0, writers = 0;
    uint64_t writes = 0;  // edge-triggered epoll: a new edge with every write
};
constexpr int kPipeBase = 1 << 20;
inline std::mutex g_pipes_lock;
inline std::vector<std::shared_ptr<Pipe>> g_pipes;
inline bool is_pipe(int h) { return h >= kPipeBase; }
inline std::shared_ptr<Pipe> pipe_of(int h) {
    std::lock_guard<std::mutex> lock(g_pipes_lock);
    const size_t i = size_t(h - kPipeBase) / 2;
    return i < g_pipes.size() ? g_pipes[i] : nullptr;
}
inline int pipe(int h[2]) {
    auto p = std::make_shared<Pipe>();
    p->readers = p->writers = 1;
    std::lock_guard<std::mutex> lock(g_pipes_lock);
    g_pipes.push_back(p);
    h[0] = kPipeBase + int(g_pipes.size() - 1) * 2;
    h[1] = h[0] + 1;
    return 0;
}
inline bool write_end(int h) { return (h - kPipeBase) & 1; }
inline int open(const char *p, int flags) { return ::open(p, flags, 0644); }
inline int read(int h, void *b, unsigned n) {
    if (!is_pipe(h)) return int(::read(h, b, n));
    auto p = pipe_of(h);
    if (!p || write_end(h)) { errno = EBADF; return -1; }
    std::unique_lock<std::mutex> lock(p->lock);
    // in slices: a wakeup lost in the console's condition variables (seen on the 3DS) costs 100 ms
    while (p->data.empty() && p->writers != 0) p->cv.wait_for(lock, std::chrono::milliseconds(100));
    const unsigned k = std::min<unsigned>(n, unsigned(p->data.size()));
    std::copy_n(p->data.begin(), k, static_cast<char *>(b));
    p->data.erase(p->data.begin(), p->data.begin() + k);
    return int(k);
}
inline int write(int h, const void *b, unsigned n) {
    if (!is_pipe(h)) return int(::write(h, b, n));
    auto p = pipe_of(h);
    if (!p || !write_end(h)) { errno = EBADF; return -1; }
    std::lock_guard<std::mutex> lock(p->lock);
    if (p->readers == 0) { errno = EPIPE; return -1; }
    p->data.insert(p->data.end(), static_cast<const char *>(b), static_cast<const char *>(b) + n);
    p->writes++;
    p->cv.notify_all();
    return int(n);
}
inline int64_t lseek(int h, int64_t off, int whence) {
    if (is_pipe(h)) { errno = ESPIPE; return -1; }
    return ::lseek(h, off_t(off), whence);
}
inline int close(int h) {
    if (!is_pipe(h)) return ::close(h);
    auto p = pipe_of(h);
    if (!p) { errno = EBADF; return -1; }
    std::lock_guard<std::mutex> lock(p->lock);
    (write_end(h) ? p->writers : p->readers)--;
    p->cv.notify_all();
    return 0;
}
inline int dup(int h) {
    if (!is_pipe(h)) return ::dup(h);
    auto p = pipe_of(h);
    if (!p) { errno = EBADF; return -1; }
    std::lock_guard<std::mutex> lock(p->lock);
    (write_end(h) ? p->writers : p->readers)++;
    return h;  // the same end, counted once more
}
inline int unlink(const char *p) { return ::unlink(p); }
inline int stat(const char *p, stat_t *s) { return ::stat(p, s); }
inline int fstat(int h, stat_t *s) {
    if (!is_pipe(h)) return ::fstat(h, s);
    *s = stat_t{};
    s->st_mode = S_IFIFO | 0600;
    return 0;
}
inline bool is_dir(const stat_t &s) { return S_ISDIR(s.st_mode); }
inline bool pipe_readable(int h) {
    auto p = pipe_of(h);
    if (!p) return true;
    std::lock_guard<std::mutex> lock(p->lock);
    return !p->data.empty() || p->writers == 0;
}
#define NOVA3_PIPE_EDGES 1
inline uint64_t pipe_writes(int h) {
    auto p = pipe_of(h);
    if (!p) return 0;
    std::lock_guard<std::mutex> lock(p->lock);
    return p->writes;
}
inline int fileno(FILE *f) { return ::fileno(f); }
inline FILE *fdopen(int h, const char *mode) { return ::fdopen(h, mode); }
inline void lock_file(FILE *f) { ::flockfile(f); }
inline void unlock_file(FILE *f) { ::funlockfile(f); }
constexpr int kRdOnly = O_RDONLY, kWrOnly = O_WRONLY, kRdWr = O_RDWR, kCreat = O_CREAT, kExcl = O_EXCL,
              kTrunc = O_TRUNC, kAppend = O_APPEND;
#else
using stat_t = struct ::stat;
inline int open(const char *p, int flags) { return ::open(p, flags, 0644); }
inline int read(int h, void *b, unsigned n) { return int(::read(h, b, n)); }
inline int write(int h, const void *b, unsigned n) { return int(::write(h, b, n)); }
inline int64_t lseek(int h, int64_t off, int whence) { return ::lseek(h, off_t(off), whence); }
inline int close(int h) { return ::close(h); }
inline int dup(int h) { return ::dup(h); }
inline int unlink(const char *p) { return ::unlink(p); }
inline int stat(const char *p, stat_t *s) { return ::stat(p, s); }
inline int fstat(int h, stat_t *s) { return ::fstat(h, s); }
inline bool is_dir(const stat_t &s) { return S_ISDIR(s.st_mode); }
inline int pipe(int h[2]) { return ::pipe(h); }
inline int fileno(FILE *f) { return ::fileno(f); }
inline FILE *fdopen(int h, const char *mode) { return ::fdopen(h, mode); }
inline void lock_file(FILE *f) { ::flockfile(f); }
inline void unlock_file(FILE *f) { ::funlockfile(f); }
constexpr int kRdOnly = O_RDONLY, kWrOnly = O_WRONLY, kRdWr = O_RDWR, kCreat = O_CREAT, kExcl = O_EXCL,
              kTrunc = O_TRUNC, kAppend = O_APPEND;
inline bool is_pipe(int h) {
    struct ::stat s;
    return ::fstat(h, &s) == 0 && S_ISFIFO(s.st_mode);
}
inline bool pipe_readable(int h) {
    pollfd p{h, POLLIN, 0};
    return ::poll(&p, 1, 0) != 0;
}
#endif
}  // namespace hio

namespace rt {
namespace {

using u32 = uint32_t;

FsConfig g_cfg;
std::string g_cwd = "/";
std::mutex g_io_lock;

int map_host_errno(int e) {
    switch (e) {
    case ENOENT: return kENOENT;
    case EACCES: return kEACCES;
    case EEXIST: return kEEXIST;
    case EBADF: return kEBADF;
    case EINVAL: return kEINVAL;
    case ENOTDIR: return kENOTDIR;
    case EISDIR: return kEISDIR;
    case ENOSPC: return kENOSPC;
    case EMFILE: return kEMFILE;
    case ENOTEMPTY: return kENOTEMPTY;
    default: return kEIO;
    }
}
void set_host_errno() { set_errno(map_host_errno(errno)); }

std::string normalize(const std::string &in) {
    std::string p = in.empty() || in[0] != '/' ? g_cwd + "/" + in : in;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    std::string out;
    for (auto &s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}

void write_file(const std::string &path, const std::string &content) {
#ifdef __3DS__
    // (on the SD card a write is several requests, ~20 ms each in Azahar: the 3DS's generated files
    // never change, so one an earlier start wrote is kept when its size is the same)
    std::error_code ec;
    if (fs::file_size(path, ec) == content.size() && !ec) return;
#endif
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

}  // namespace

#ifdef __3DS__
namespace {
void list_data_dir();
void load_index();
struct Packed;
}
#endif

bool low_end_device() {
#if defined(__3DS__) || defined(__GAMECUBE__)
    // (the GameCube: MEM_256 for the 24 MB of MEM1 + 16 MB of ARAM the guest's memory pages
    // through, and for now GPU_0, whose shaders the GX materials know: docs/gamecube.md)
    return true;
#else
    static const bool low = [] { const char *d = std::getenv("NOVA3_DEVICE"); return d && std::strcmp(d, "3ds") == 0; }();
    return low;
#endif
}

void init_fs(const FsConfig &cfg) {
    g_cfg = cfg;
    std::string root = cfg.data_dir + "/root";
    fs::create_directories(cfg.data_dir + "/sdcard");
    fs::create_directories(cfg.save_dir.empty() ? root + "/data/data/" + cfg.package + "/files" : cfg.save_dir + "/files");
    fs::create_directories(root + "/data/data/" + cfg.package + "/cache");
    fs::create_directories(root + "/tmp");
    // Android CPU description: ARMv7 Cortex-A9-class, VFPv3, no NEON.
    const bool low = low_end_device();
    const int cores = low ? 1 : 4;
    const char *mhz = low ? "804000\n" : "1500000\n";
    std::string cpuinfo = "Processor\t: ARMv7 Processor rev 10 (v7l)\n";
    for (int i = 0; i < cores; i++) cpuinfo += "processor\t: " + std::to_string(i) + "\nBogoMIPS\t: 1500.00\n\n";
    cpuinfo += "Features\t: swp half thumb fastmult vfp edsp vfpv3 vfpv3d16 tls\n"
               "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x2\nCPU part\t: 0xc09\n"
               "CPU revision\t: 10\n\nHardware\t: Windows PC\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
    write_file(root + "/proc/cpuinfo", cpuinfo);
    write_file(root + "/proc/meminfo", low ? "MemTotal:         262144 kB\nMemFree:          131072 kB\n"
                                             "Buffers:           16384 kB\nCached:            65536 kB\n"
                                           : "MemTotal:        2097152 kB\nMemFree:         1048576 kB\n"
                                             "Buffers:           65536 kB\nCached:           524288 kB\n");
    // the game counts the cpuN directories too: drop those of earlier runs with another count
    std::error_code ec;
#ifdef __3DS__
    if (fs::exists(root + "/sys/devices/system/cpu/cpu" + std::to_string(cores), ec))  // (always one core here)
#endif
    fs::remove_all(root + "/sys/devices/system/cpu", ec);
    const std::string range = cores == 1 ? "0\n" : "0-" + std::to_string(cores - 1) + "\n";
    write_file(root + "/sys/devices/system/cpu/present", range);
    write_file(root + "/sys/devices/system/cpu/possible", range);
    for (int i = 0; i < cores; i++)
        write_file(root + "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq", mhz);
#ifdef __3DS__
    list_data_dir();
    load_index();
#endif
}

std::string host_path(const char *guest) {
    std::string p = normalize(guest ? guest : "");
    if (p == "/data/app/" + g_cfg.package + "-1.apk") return g_cfg.apk_path;
    if (!g_cfg.save_dir.empty()) {  // the app's own data (files/, shared_prefs/), but not cache/
        const std::string app = "/data/data/" + g_cfg.package, cache = app + "/cache";
        bool in_app = p == app || p.compare(0, app.size() + 1, app + "/") == 0;
        bool in_cache = p == cache || p.compare(0, cache.size() + 1, cache + "/") == 0;
        if (in_app && !in_cache) return g_cfg.save_dir + p.substr(app.size());
    }
    const std::string obb = "/Android/obb/" + g_cfg.package;
    const char *sd_roots[] = {"/sdcard", "/mnt/sdcard", "/storage/emulated/0", "/storage/sdcard0"};
    for (const char *r : sd_roots) {
        std::string root = r;
        if (p == root || p.compare(0, root.size() + 1, root + "/") == 0) {
            std::string rest = p.substr(root.size());
            if (rest.compare(0, obb.size(), obb) == 0) return g_cfg.obb_dir + rest.substr(obb.size());
            return g_cfg.data_dir + "/sdcard" + rest;
        }
    }
    return g_cfg.data_dir + "/root" + p;
}

namespace {

// ---- fd table ---------------------------------------------------------------------
constexpr int kDirFd = -2;  // directory opened with open(): only fstat/close work
std::vector<int> g_fds = {0, 1, 2};
std::vector<std::string> g_fd_paths = {"", "", ""};
std::vector<bool> g_nonblock;
constexpr int kEpollFd = -3;
constexpr int kSocketFd = -4;  // socket: the SOCKET lives in net.cpp, keyed by guest fd
#ifdef __3DS__
constexpr int kPackedFd = -5;  // a packed game file (below): the file and the position here
struct PackedFd {
    Packed *p;
    int64_t pos;
};
std::unordered_map<int, PackedFd> g_packed_fds;  // under g_io_lock
#endif
struct EpollEntry {
    u32 events;
    uint64_t data;
    bool out_pending;
    uint64_t seen_writes = ~0ull;  // EPOLLET on an emulated pipe: the write count last reported
};
std::unordered_map<int, std::unordered_map<int, EpollEntry>> g_epolls;  // epoll fd -> watched fds

int fd_alloc(int host, const std::string &path) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    for (size_t i = 3; i < g_fds.size(); i++)
        if (g_fds[i] == -1) { g_fds[i] = host; g_fd_paths[i] = path; return int(i); }
    g_fds.push_back(host);
    g_fd_paths.push_back(path);
    return int(g_fds.size() - 1);
}
int host_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    return fd >= 0 && size_t(fd) < g_fds.size() ? g_fds[fd] : -1;
}
#ifdef __3DS__
int packed_fd(Packed *p, const std::string &host, int64_t pos = 0) {
    const int fd = fd_alloc(kPackedFd, host);
    std::lock_guard<std::mutex> lock(g_io_lock);
    g_packed_fds[fd] = {p, pos};
    return fd;
}
bool packed_fd_of(int fd, PackedFd &d) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    auto it = g_packed_fds.find(fd);
    if (it == g_packed_fds.end()) { set_errno(kEBADF); return false; }
    d = it->second;
    return true;
}
#endif

// bionic struct stat (== ARM stat64, 104 bytes)
void fill_stat(u32 st, bool dir, uint64_t size, int64_t mtime) {
    std::memset(ptr(st), 0, 104);
    ST32(st + 16, dir ? 0040755u : 0100644u);
    ST32(st + 20, 1);
    ST64(st + 48, size);
    ST32(st + 56, 4096);
    ST64(st + 64, (size + 511) / 512);
    for (u32 off : {72u, 80u, 88u}) ST32(st + off, u32(mtime));
}

// The file system requests behind the game's file calls (the 3DS's loading lines and load
// profile): host reads (the read cache's misses and large reads), opens (a probe for a missing
// file is one too) and stats, with their time in os::ticks; on the 3DS also the blocks of packed
// files unpacked (bytes, time). g_io_now: the one the game thread is waiting for (1 read, 2 open,
// 3 stat, 4 unpacking), for the 3DS's load profile.
std::atomic<uint64_t> g_host_read_bytes{0}, g_host_reads{0}, g_host_read_ticks{0};
std::atomic<uint64_t> g_host_opens{0}, g_host_open_ticks{0}, g_host_stats{0}, g_host_stat_ticks{0};
std::atomic<uint64_t> g_unpacked_bytes{0}, g_unpack_ticks{0};
#ifdef __3DS__
std::atomic<uint64_t> g_unpacks{0};
#endif
std::atomic<int> g_io_now{0};
struct HostRequest {
    std::atomic<uint64_t> &count, &ticks;
    const uint64_t t0;
    const bool game;
    HostRequest(int what, std::atomic<uint64_t> &c, std::atomic<uint64_t> &t)
        : count(c), ticks(t), t0(os::ticks()), game(current_cpu() && current_cpu() == main_cpu()) {
        if (game) g_io_now = what;
    }
    ~HostRequest() {
        ticks += os::ticks() - t0;
        count++;
        if (game) g_io_now = 0;
    }
};
enum { kIoRead = 1, kIoOpen, kIoStat, kIoUnpack };

int stat_host(const std::string &hp, u32 st) {
    hio::stat_t s;
    int r;
    {
        HostRequest req(kIoStat, g_host_stats, g_host_stat_ticks);
        r = hio::stat(hp.c_str(), &s);
    }
    if (r != 0) { set_host_errno(); return -1; }
    fill_stat(st, hio::is_dir(s), uint64_t(s.st_size), s.st_mtime);
    return 0;
}

// Paths the guest looked for and did not find. The game probes for loose files before reading
// its archives (a chapter-0 load: ~12,800 misses out of 14,500 fopens, the same few hundred
// names again and again), and on the consoles every probe is a request to the file system
// service (the 3DS's SD card; ~0.2 ms each on the Switch; on a PC about 0.2 s a load). A known
// miss fails at once; anything that creates, renames or removes files forgets them all.
std::mutex g_missing_lock;
std::unordered_set<std::string> g_missing;
#ifdef __3DS__
// The first probe of each name was still a request, and on the 3DS an open or a stat is the
// costliest one (~17 ms each in Azahar: a chapter-3 load made ~450 probes, a quarter of its time).
// Most need none:
//  * the game files' folder never changes: its listing (lower-case names, as FAT compares them),
//    taken once, answers for any name in it that is not there, and for anything "inside" one of
//    its files (the game probes "weapons_stream.gla/x_low.bdae");
//  * the other probes go to a few folders that do not exist (/sdcard/android/obb/<package>/...
//    in lower case: 150 names a load): after a second miss in a folder, one request finds out
//    whether the folder is there, and a missing folder's names fail at once.
// What a write can make appear is forgotten (forget_missing), and a change inside the game
// files' folder drops its listing.
std::unordered_map<std::string, bool> g_data_names;  // the game files' folder: lower-case name -> is a folder
bool g_data_listed = false;
std::unordered_set<std::string> g_missing_dirs, g_present_dirs;

std::string lower(std::string s) {
    for (char &ch : s) ch = char(std::tolower(uint8_t(ch)));
    return s;
}
bool in_data_dir(const std::string &host) {
    const std::string &d = g_cfg.obb_dir;
    return !d.empty() && host.size() > d.size() + 1 && host.compare(0, d.size(), d) == 0 && host[d.size()] == '/';
}
// Packed game files (tools/3ds_slimdata.py pack): the OBB-format archives (*_stream.gla,
// sounds_*.gla), which the game reads uncompressed, are on the SD card as X.gla.z, deflated in
// blocks of 64 KB (the streams) or 32 KB (the sounds), 0.53 and 0.83 of their size. The game opens
// X.gla, reads, seeks and asks its size as before, through stdio or a descriptor, and gets the
// original's bytes and size: the blocks it reads are unpacked into a pool of recent ones (below).
// Where X.gla is there too, it is read instead. The container stays open from its first use on, so
// opening X.gla again costs no request. Format: "N3BZ", version 1, the original's size, the block
// size, the block count n, the original's CRC-32 (u32 each); n + 1 u32 offsets (block k is [at[k],
// at[k + 1]) of the container); n u32 CRC-32s of the blocks' original bytes; the blocks (raw
// deflate, or as they are where their stored size is their size).
struct Packed {
    std::string path;               // the container's host path
    enum { kNew, kReady, kBad } state = kNew;
    uint32_t size = 0, block = 0;   // the original's size, the block size
    int64_t mtime = 0;
    std::vector<uint32_t> at, crc;  // the blocks' offsets in the container, their CRC-32s
    std::mutex lock;                // reads of the file: its descriptor and inflater; loading the above
    int h = -1;                     // the container's descriptor
    z_stream z{};
    bool z_ready = false;
};
std::unordered_map<std::string, std::unique_ptr<Packed>> g_packed;  // lower-case X.gla -> X.gla.z; set at the listing

void list_data_dir() {
    DIR *dir = opendir(g_cfg.obb_dir.c_str());
    if (!dir) return;
    std::vector<std::string> packed;  // X.gla.z files
    while (const dirent *e = readdir(dir))
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
            const std::string name = lower(e->d_name);
            g_data_names[name] = e->d_type == DT_DIR;
            if (e->d_type != DT_DIR && name.size() > 6 && name.compare(name.size() - 6, 6, ".gla.z") == 0) packed.push_back(e->d_name);
        }
    closedir(dir);
    for (const std::string &n : packed) {
        const std::string plain = lower(n.substr(0, n.size() - 2));
        if (g_data_names.count(plain)) continue;  // (X.gla itself is read)
        g_data_names[plain] = false;
        g_packed[plain] = std::make_unique<Packed>();
        g_packed[plain]->path = g_cfg.obb_dir + "/" + n;
    }
    if (!g_packed.empty()) log("files: %zu packed archives", g_packed.size());
    g_data_listed = !g_data_names.empty();
}
// a path in the game files' folder that cannot be there (by its listing)
bool data_dir_lacks(const std::string &host) {
    if (!g_data_listed || !in_data_dir(host)) return false;
    const size_t start = g_cfg.obb_dir.size() + 1, slash = host.find('/', start);
    auto it = g_data_names.find(lower(host.substr(start, slash == std::string::npos ? std::string::npos : slash - start)));
    return it == g_data_names.end() || (slash != std::string::npos && !it->second);
}
std::string parent_dir(const std::string &host) {
    const size_t slash = host.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string() : host.substr(0, slash);
}
bool is_folder(const std::string &dir) {
    hio::stat_t s;
    HostRequest req(kIoStat, g_host_stats, g_host_stat_ticks);
    return hio::stat(dir.c_str(), &s) == 0 && hio::is_dir(s);
}
// n bytes at pos of a host descriptor (fewer at its end), in one request
size_t fd_read_at(int h, int64_t pos, void *dst, size_t n) {
    if (hio::lseek(h, pos, SEEK_SET) != pos) return 0;
    size_t done = 0;
    while (done < n) {
        const int r = hio::read(h, static_cast<uint8_t *>(dst) + done, unsigned(n - done));
        if (r <= 0) break;
        done += size_t(r);
    }
    return done;
}

// ---- packed game files (above: the listing) ----
void load_packed(Packed &p) {
    p.state = Packed::kBad;
    {
        HostRequest req(kIoOpen, g_host_opens, g_host_open_ticks);
        p.h = hio::open(p.path.c_str(), hio::kRdOnly);
    }
    if (p.h < 0) {
        log("files: cannot open %s", p.path.c_str());
        return;
    }
    uint32_t head[6] = {};
    bool ok;
    {
        HostRequest req(kIoRead, g_host_reads, g_host_read_ticks);
        ok = fd_read_at(p.h, 0, head, sizeof head) == sizeof head;
    }
    const uint32_t n = head[4];
    ok = ok && !std::memcmp(head, "N3BZ", 4) && head[1] == 1 && head[3] >= 4096 && head[3] <= (1u << 20) &&
         n == uint32_t((uint64_t(head[2]) + head[3] - 1) / head[3]);
    std::vector<uint32_t> tables(ok ? 2 * size_t(n) + 1 : 0);
    if (ok) {
        HostRequest req(kIoRead, g_host_reads, g_host_read_ticks);
        ok = fd_read_at(p.h, sizeof head, tables.data(), tables.size() * 4) == tables.size() * 4;
        g_host_read_bytes += sizeof head + tables.size() * 4;
    }
    hio::stat_t st{};
    if (ok) {
        HostRequest req(kIoStat, g_host_stats, g_host_stat_ticks);
        ok = hio::fstat(p.h, &st) == 0 && uint64_t(st.st_size) == tables[n];
    }
    // every block where the tables say, no larger than its original bytes
    for (uint32_t k = 0; ok && k < n; k++)
        ok = tables[k + 1] >= tables[k] && tables[k + 1] - tables[k] <= std::min(head[3], head[2] - k * head[3]);
    if (!ok || tables[0] != sizeof head + tables.size() * 4) {
        log("files: %s is damaged (its tables): not used", p.path.c_str());
        hio::close(p.h);
        p.h = -1;
        return;
    }
    p.size = head[2], p.block = head[3], p.mtime = int64_t(st.st_mtime);
    p.at.assign(tables.begin(), tables.begin() + n + 1);
    p.crc.assign(tables.begin() + n + 1, tables.end());
    p.state = Packed::kReady;
}
// The packed file a game file's host path names (its directory loaded at the first use), or nullptr
Packed *packed_file(const std::string &host) {
    if (g_packed.empty() || !in_data_dir(host)) return nullptr;
    const size_t start = g_cfg.obb_dir.size() + 1;
    if (host.find('/', start) != std::string::npos) return nullptr;
    auto it = g_packed.find(lower(host.substr(start)));
    if (it == g_packed.end()) return nullptr;
    Packed &p = *it->second;
    std::lock_guard<std::mutex> lock(p.lock);
    if (p.state == Packed::kNew) load_packed(p);
    return p.state == Packed::kReady ? &p : nullptr;
}
// block k of a packed file, unpacked into dst and checked (its CRC-32); p.lock held
bool unpack(Packed &p, uint32_t k, uint8_t *dst) {
    const uint32_t len = std::min(p.block, p.size - k * p.block), stored = p.at[k + 1] - p.at[k];
    static RT_THREAD_LOCAL(std::vector<uint8_t>, in_slots);
    std::vector<uint8_t> &in = RT_TLS(in_slots);
    if (stored != len && in.size() < stored) in.resize(stored);
    uint8_t *src = stored == len ? dst : in.data();
    bool ok;
    {
        HostRequest req(kIoRead, g_host_reads, g_host_read_ticks);
        ok = fd_read_at(p.h, p.at[k], src, stored) == stored;
    }
    g_host_read_bytes += stored;
    HostRequest req(kIoUnpack, g_unpacks, g_unpack_ticks);
    if (ok && stored != len) {
        if (!p.z_ready) p.z_ready = inflateInit2(&p.z, -15) == Z_OK;
        else inflateReset(&p.z);
        p.z.next_in = src, p.z.avail_in = stored, p.z.next_out = dst, p.z.avail_out = len;
        ok = p.z_ready && inflate(&p.z, Z_FINISH) == Z_STREAM_END && p.z.avail_out == 0;
    }
    if (ok && crc32(0, dst, len) != p.crc[k]) ok = false;
    if (!ok) log("files: %s is damaged at block %u (offset %u)", p.path.c_str(), k, k * p.block);
    g_unpacked_bytes += len;
    return ok;
}
// Unpacked blocks, of every packed file: the least recently read one is reused (16 at most: 1 MB). The
// game reads a member of an archive in pieces, the sounds 1 KB at a time, a few files at once (the
// level's streams while sounds play): simulated on recorded loads, 16 blocks shared do as well as 4 per
// file, and unpack 1.5-2 times the bytes read (chapter 3's load: 20 MB). A block is unpacked under
// its file's lock only (busy meanwhile); the pool's lock is held for lookups and copies.
struct PackedBlock {
    const Packed *owner = nullptr;
    uint32_t index = 0;
    bool busy = false;
    std::vector<uint8_t> data;
};
std::mutex g_pblocks_lock;
std::list<PackedBlock> g_pblocks;  // most recent first
constexpr size_t kPackedBlocks = 16;

// [pos, pos + n) of a packed file's original bytes: the bytes read (fewer at its end, or at a damaged block)
size_t packed_read(Packed &p, int64_t pos, uint8_t *dst, size_t n) {
    if (pos < 0 || pos >= int64_t(p.size)) return 0;
    n = size_t(std::min<int64_t>(int64_t(n), int64_t(p.size) - pos));
    std::lock_guard<std::mutex> file(p.lock);
    size_t done = 0;
    while (done < n) {
        const int64_t at = pos + int64_t(done);
        const uint32_t k = uint32_t(at / p.block), off = uint32_t(at % p.block), len = std::min(p.block, p.size - k * p.block);
        std::unique_lock<std::mutex> lock(g_pblocks_lock);
        auto it = std::find_if(g_pblocks.begin(), g_pblocks.end(),
                               [&p, k](const PackedBlock &b) { return b.owner == &p && b.index == k && !b.busy; });
        if (it == g_pblocks.end()) {
            if (!off && n - done >= len) {  // a whole block: straight into the reader's buffer
                lock.unlock();
                if (!unpack(p, k, dst + done)) break;
                done += len;
                continue;
            }
            auto reuse = g_pblocks.end();  // the least recent block not being unpacked
            if (g_pblocks.size() >= kPackedBlocks)
                for (auto b = g_pblocks.end(); b != g_pblocks.begin();)
                    if (!(--b)->busy) { reuse = b; break; }
            if (reuse == g_pblocks.end()) g_pblocks.emplace_front();
            else g_pblocks.splice(g_pblocks.begin(), g_pblocks, reuse);
            it = g_pblocks.begin();
            it->owner = &p, it->index = k, it->busy = true;
            lock.unlock();
            it->data.resize(len);
            const bool ok = unpack(p, k, it->data.data());
            lock.lock();
            it->busy = false;
            if (!ok) {
                it->owner = nullptr;
                break;
            }
        } else if (it != g_pblocks.begin()) {
            g_pblocks.splice(g_pblocks.begin(), g_pblocks, it);
        }
        const size_t take = std::min<size_t>(n - done, len - off);
        std::memcpy(dst + done, it->data.data() + off, take);
        done += take;
    }
    return done;
}
// A host FILE reading a packed file's original (fopencookie): for the stdio calls on the guest's FILE
// other than fread / fseek / ftell (those go to the read cache, which reads packed files directly).
struct PackedCookie {
    Packed *p;
    int64_t pos;
};
FILE *packed_stdio(Packed *p, int64_t pos = 0) {
    cookie_io_functions_t io{};
    io.read = [](void *c, char *buf, size_t n) -> ssize_t {
        auto *k = static_cast<PackedCookie *>(c);
        const size_t got = packed_read(*k->p, k->pos, reinterpret_cast<uint8_t *>(buf), n);
        k->pos += int64_t(got);
        return ssize_t(got);
    };
    io.seek = [](void *c, _off64_t *off, int whence) -> int {
        auto *k = static_cast<PackedCookie *>(c);
        const int64_t to = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? k->pos : int64_t(k->p->size)) + int64_t(*off);
        if (to < 0) return -1;
        *off = k->pos = to;
        return 0;
    };
    io.close = [](void *c) -> int {
        delete static_cast<PackedCookie *>(c);
        return 0;
    };
    auto *cookie = new PackedCookie{p, pos};
    FILE *f = fopencookie(cookie, "rb", io);
    if (!f) delete cookie;
    return f;
}
#endif
bool known_missing(const std::string &host) {
#ifdef __3DS__
    if (data_dir_lacks(host)) return true;
#endif
    std::lock_guard<std::mutex> lock(g_missing_lock);
#ifdef __3DS__
    if (!g_missing_dirs.empty() && g_missing_dirs.count(parent_dir(host))) return true;
#endif
    return g_missing.count(host) != 0;
}
#ifdef __3DS__
// Folders with a miss so far (one is checked at its second: most have one or two)
std::unordered_map<std::string, int> g_dir_misses;
#endif
// remembers a failed lookup (true) if the file was not there (errno ENOENT, which stays as it was)
bool note_missing(const std::string &host) {
    const int e = errno;
    if (e != ENOENT) return false;
    {
        std::lock_guard<std::mutex> lock(g_missing_lock);
        if (g_missing.size() >= 16384) g_missing.clear();
        g_missing.insert(host);
    }
#ifdef __3DS__
    const std::string dir = parent_dir(host);
    {
        std::lock_guard<std::mutex> lock(g_missing_lock);
        if (dir.empty() || g_present_dirs.count(dir) || g_missing_dirs.count(dir)) return true;
        if (g_dir_misses.size() >= 4096) g_dir_misses.clear();
        if (++g_dir_misses[dir] < 2) return true;
    }
    const bool present = is_folder(dir);  // (a file holds nothing either)
    errno = e;
    std::lock_guard<std::mutex> lock(g_missing_lock);
    (present ? g_present_dirs : g_missing_dirs).insert(dir);
#endif
    return true;
}
#ifdef __3DS__
// The same misses by the guest's own path: the game probes the same ~500 names ~17,000 times a
// level load, and mapping each to a host path (host_path: some 45 string allocations) took ~125 us
// in Azahar, 7% of a load. Forgotten with the host paths' misses and when the guest changes its
// current folder.
struct PathHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
std::unordered_set<std::string, PathHash, std::equal_to<>> g_guest_missing;  // under g_missing_lock
bool guest_known_missing(u32 guest) {
    const char *p = str(guest);
    if (!p) return false;
    std::lock_guard<std::mutex> lock(g_missing_lock);
    return !g_guest_missing.empty() && g_guest_missing.find(std::string_view(p)) != g_guest_missing.end();
}
void note_guest_missing(u32 guest) {
    const char *p = str(guest);
    if (!p) return;
    std::lock_guard<std::mutex> lock(g_missing_lock);
    if (g_guest_missing.size() >= 16384) g_guest_missing.clear();
    g_guest_missing.emplace(p);
}
#else
bool guest_known_missing(u32) { return false; }
void note_guest_missing(u32) {}
#endif
// `host` is created, renamed or removed (empty: not known). On the 3DS only what it can make
// appear is forgotten (the path, what is under it, the folders above it): the game writes its
// save and analytics files during loads, and each write forgot every miss.
void forget_missing(const std::string &host = {}) {
    std::lock_guard<std::mutex> lock(g_missing_lock);
#ifdef __3DS__
    g_guest_missing.clear();
    g_dir_misses.clear();
    if (host.empty() || in_data_dir(host)) g_data_listed = false;
    if (!host.empty()) {
        const auto related = [&host](const std::string &p) {
            const std::string &a = p.size() < host.size() ? p : host, &b = p.size() < host.size() ? host : p;
            return b.compare(0, a.size(), a) == 0 && (b.size() == a.size() || b[a.size()] == '/');
        };
        for (auto *set : {&g_missing, &g_missing_dirs})
            for (auto it = set->begin(); it != set->end();) it = related(*it) ? set->erase(it) : std::next(it);
        return;
    }
    g_missing_dirs.clear();
    g_present_dirs.clear();
#else
    (void)host;
#endif
    g_missing.clear();
}

// NOVA3_TRACE_FILES=1: log the game's file lookups (guest path, host path, result).
bool tracing_files() {
    static const bool on = std::getenv("NOVA3_TRACE_FILES") != nullptr;
    return on;
}
void trace_file(const char *op, u32 guest, const std::string &host, bool ok) {
    if (tracing_files()) log("file: %s %s -> %s%s", op, str(guest), host.c_str(), ok ? "" : " (missing)");
}
// a lookup that needs no host path: a known miss by the guest's path (not while tracing lookups)
bool quick_miss(u32 guest) {
    if (tracing_files() || !guest_known_missing(guest)) return false;
    set_errno(kENOENT);
    return true;
}
// NOVA3_TRACE_READS=1: log every read the game makes (host path, offset, bytes), to see which
// parts of its archives a load reads (tools/3ds_slimdata.py measure)
bool tracing_reads() {
    static const bool on = std::getenv("NOVA3_TRACE_READS") != nullptr;
    return on;
}
void trace_read(int fd, int64_t pos, uint64_t n) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        if (fd >= 0 && size_t(fd) < g_fd_paths.size()) path = g_fd_paths[fd];
    }
    if (n) log("read: %s %lld %llu", path.c_str(), static_cast<long long>(pos), static_cast<unsigned long long>(n));
}

#ifdef __3DS__
// read (off < 0: at the descriptor's position, which moves) or pread of a packed file's descriptor
int packed_fd_read(int fd, u32 buf, u32 n, int64_t off) {
    PackedFd d;
    if (!packed_fd_of(fd, d)) return -1;
    const int64_t at = off < 0 ? d.pos : off;
    const size_t got = packed_read(*d.p, at, reinterpret_cast<uint8_t *>(ptr(buf)), n);
    if (off < 0) {
        std::lock_guard<std::mutex> lock(g_io_lock);
        if (auto it = g_packed_fds.find(fd); it != g_packed_fds.end()) it->second.pos = at + int64_t(got);
    }
    if (tracing_reads()) trace_read(fd, at, got);
    return int(got);
}
#endif
int x_open(Cpu *cpu, u32 path, int flags, u32 mode) {
    (void)cpu;
    const bool creates = (flags & 3) != 0 || (flags & 0x40);  // writes, O_CREAT
    if (!creates && quick_miss(path)) return -1;
    std::string hp = host_path(str(path));
    if (creates) forget_missing(hp);
    else if (known_missing(hp)) { note_guest_missing(path); set_errno(kENOENT); return -1; }
#ifdef __3DS__
    else if (Packed *p = packed_file(hp)) {
        trace_file("open", path, hp, true);
        return packed_fd(p, hp);
    }
#endif
    std::error_code ec;
    bool dir;
    {
        HostRequest req(kIoStat, g_host_stats, g_host_stat_ticks);
        dir = fs::is_directory(hp, ec);
    }
    if (dir) {
        if ((flags & 3) != 0) { set_errno(kEISDIR); return -1; }
        return fd_alloc(kDirFd, hp);
    }
    if (flags & 0x4000) { set_errno(kENOTDIR); return -1; }  // O_DIRECTORY
    int hf = 0;
    hf |= (flags & 3) == 0 ? hio::kRdOnly : (flags & 3) == 1 ? hio::kWrOnly : hio::kRdWr;
    if (flags & 0x40) hf |= hio::kCreat;
    if (flags & 0x80) hf |= hio::kExcl;
    if (flags & 0x200) hf |= hio::kTrunc;
    if (flags & 0x400) hf |= hio::kAppend;
    if (flags & 0x40) fs::create_directories(fs::path(hp).parent_path(), ec);
    int h;
    {
        HostRequest req(kIoOpen, g_host_opens, g_host_open_ticks);
        h = hio::open(hp.c_str(), hf);
    }
    (void)mode;
    if (h < 0 && !creates && note_missing(hp)) note_guest_missing(path);
    trace_file("open", path, hp, h >= 0);
    if (h < 0) {
        set_host_errno();
        return -1;
    }
    return fd_alloc(h, hp);
}
int x_close(int fd) {
    int h = host_fd(fd);
    if (h == -1) { set_errno(kEBADF); return -1; }
    if (fd > 2 && h >= 0) hio::close(h);
    if (h == kSocketFd) net_close(fd);
    std::lock_guard<std::mutex> lock(g_io_lock);
    if (fd > 2) g_fds[fd] = -1;
    if (size_t(fd) < g_nonblock.size()) g_nonblock[fd] = false;
    g_epolls.erase(fd);
#ifdef __3DS__
    g_packed_fds.erase(fd);
#endif
    return 0;
}
// ---- readiness / O_NONBLOCK (pipes are the only non-file fds the game creates) ------------
constexpr u32 kONonblock = 0x800;

bool readable(int h) { return h < 0 || !hio::is_pipe(h) || hio::pipe_readable(h); }
bool nonblocking(int fd) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    return fd >= 0 && size_t(fd) < g_nonblock.size() && g_nonblock[fd];
}

int x_read(int fd, u32 buf, u32 n) {
    int h = host_fd(fd);
    if (h == kSocketFd) return net_recv(fd, buf, n, 0);
#ifdef __3DS__
    if (h == kPackedFd) return packed_fd_read(fd, buf, n, -1);
#endif
    if (h < 0) { set_errno(h == kDirFd ? kEISDIR : kEBADF); return -1; }
    if (nonblocking(fd) && !readable(h)) { set_errno(kEAGAIN); return -1; }
    const long at = tracing_reads() ? hio::lseek(h, 0, SEEK_CUR) : 0;
    int r = hio::read(h, ptr(buf), n);
    if (r < 0) set_host_errno();
    else if (tracing_reads()) trace_read(fd, at, u32(r));
    return r;
}
int x_write(int fd, u32 buf, u32 n) {
    int h = host_fd(fd);
    if (h == kSocketFd) return net_send(fd, buf, n, 0);
    if (h < 0) { set_errno(kEBADF); return -1; }
    int r = hio::write(h, ptr(buf), n);
    if (r < 0) set_host_errno();
    return r;
}
int x_writev(int fd, u32 iov, int cnt) {
    int total = 0;
    for (int i = 0; i < cnt; i++) {
        int r = x_write(fd, LD32(iov + 8 * i), LD32(iov + 8 * i + 4));
        if (r < 0) return total ? total : -1;
        total += r;
    }
    return total;
}
int32_t x_lseek(int fd, int32_t off, int whence) {
    int h = host_fd(fd);
#ifdef __3DS__
    if (h == kPackedFd) {
        std::lock_guard<std::mutex> lock(g_io_lock);
        auto it = g_packed_fds.find(fd);
        if (it == g_packed_fds.end()) { set_errno(kEBADF); return -1; }
        const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? it->second.pos : int64_t(it->second.p->size);
        if (base + off < 0) { set_errno(kEINVAL); return -1; }
        return int32_t(it->second.pos = base + off);
    }
#endif
    if (h < 0) { set_errno(kEBADF); return -1; }
    long r = hio::lseek(h, off, whence);
    if (r < 0) set_host_errno();
    return r;
}
int x_pread(int fd, u32 buf, u32 n, int32_t off) {
    int h = host_fd(fd);
#ifdef __3DS__
    if (h == kPackedFd) {
        if (off < 0) { set_errno(kEINVAL); return -1; }
        return packed_fd_read(fd, buf, n, off);
    }
#endif
    if (h < 0) { set_errno(kEBADF); return -1; }
    int r;
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        long cur = hio::lseek(h, 0, SEEK_CUR);
        hio::lseek(h, off, SEEK_SET);
        r = hio::read(h, ptr(buf), n);
        hio::lseek(h, cur, SEEK_SET);
    }
    if (r > 0 && tracing_reads()) trace_read(fd, off, u32(r));
    return r;
}
#ifdef __3DS__
// A game file's size and time never change, and the game asks for them (fstat) every time it
// opens an archive: on the 3DS each answer is a request (FSFILE_GetSize; 2.5% of a level load).
// Kept per path while the game files' folder is unchanged (its listing stands).
std::unordered_map<std::string, std::pair<uint64_t, int64_t>> g_game_file_stat;  // under g_io_lock
#endif
int x_fstat(int fd, u32 st) {
    int h = host_fd(fd);
    if (h == kDirFd) { fill_stat(st, true, 0, 0); return 0; }
#ifdef __3DS__
    if (PackedFd d; h == kPackedFd) {  // the original's size
        if (!packed_fd_of(fd, d)) return -1;
        fill_stat(st, false, d.p->size, d.p->mtime);
        return 0;
    }
#endif
    if (h < 0) { set_errno(kEBADF); return -1; }
#ifdef __3DS__
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        if (size_t(fd) < g_fd_paths.size() && g_data_listed && in_data_dir(g_fd_paths[fd])) path = g_fd_paths[fd];
        if (auto it = path.empty() ? g_game_file_stat.end() : g_game_file_stat.find(path); it != g_game_file_stat.end()) {
            fill_stat(st, false, it->second.first, it->second.second);
            return 0;
        }
    }
#endif
    hio::stat_t s;
    if (hio::fstat(h, &s) != 0) { set_host_errno(); return -1; }
    fill_stat(st, hio::is_dir(s), uint64_t(s.st_size), s.st_mtime);
#ifdef __3DS__
    if (!path.empty() && !hio::is_dir(s)) {
        std::lock_guard<std::mutex> lock(g_io_lock);
        g_game_file_stat[path] = {uint64_t(s.st_size), int64_t(s.st_mtime)};
    }
#endif
    return 0;
}
int x_stat(u32 path, u32 st) {
    if (quick_miss(path)) return -1;
    const std::string hp = host_path(str(path));
    if (known_missing(hp)) { note_guest_missing(path); set_errno(kENOENT); return -1; }
#ifdef __3DS__
    if (Packed *p = packed_file(hp)) {  // the original's size
        fill_stat(st, false, p->size, p->mtime);
        trace_file("stat", path, hp, true);
        return 0;
    }
#endif
    const int r = stat_host(hp, st);
    if (r != 0 && note_missing(hp)) note_guest_missing(path);
    trace_file("stat", path, hp, r == 0);
    return r;
}
int x_access(u32 path, int mode) {
    (void)mode;
    if (quick_miss(path)) return -1;
    const std::string hp = host_path(str(path));
    std::error_code ec;
    if (known_missing(hp)) note_guest_missing(path);
#ifdef __3DS__
    else if (packed_file(hp)) return 0;
#endif
    else {
        HostRequest req(kIoStat, g_host_stats, g_host_stat_ticks);
        if (fs::exists(hp, ec)) return 0;
    }
    set_errno(kENOENT);
    return -1;
}
int x_mkdir(u32 path, u32) {
    std::string hp = host_path(str(path));
    forget_missing(hp);
    std::error_code ec;
    if (fs::exists(hp, ec)) { set_errno(kEEXIST); return -1; }
    if (!fs::create_directory(hp, ec)) { set_errno(kENOENT); return -1; }
    return 0;
}
int x_unlink(u32 path) {
    const std::string hp = host_path(str(path));
    forget_missing(hp);
    if (hio::unlink(hp.c_str()) != 0) { set_host_errno(); return -1; }
    return 0;
}
int x_remove(u32 path) {
    const std::string hp = host_path(str(path));
    forget_missing(hp);
    std::error_code ec;
    if (!fs::remove(hp, ec)) { set_errno(kENOENT); return -1; }
    return 0;
}
int x_rename(u32 from, u32 to) {
    std::string a = host_path(str(from)), b = host_path(str(to));
    forget_missing(a);
    forget_missing(b);
    std::error_code ec;
    fs::rename(a, b, ec);  // replaces an existing target, like POSIX rename
    if (ec) { set_errno(kENOENT); return -1; }
    return 0;
}
int x_chmod(u32, u32) { return 0; }
u32 x_umask(u32) { return 022; }
int x_chdir(u32 path) {
    std::string p = normalize(str(path));
    std::error_code ec;
    if (!fs::is_directory(host_path(p.c_str()), ec)) { set_errno(kENOENT); return -1; }
    g_cwd = p;
#ifdef __3DS__
    std::lock_guard<std::mutex> lock(g_missing_lock);
    g_guest_missing.clear();  // (relative paths)
#endif
    return 0;
}
int x_dup2(int from, int to) {
    int h = host_fd(from);
#ifdef __3DS__
    if (h < 0 && h != kPackedFd) { set_errno(kEBADF); return -1; }
    int nh = h == kPackedFd ? h : hio::dup(h);
#else
    if (h < 0) { set_errno(kEBADF); return -1; }
    int nh = hio::dup(h);
#endif
    std::lock_guard<std::mutex> lock(g_io_lock);
    if (size_t(to) >= g_fds.size()) { g_fds.resize(to + 1, -1); g_fd_paths.resize(to + 1); }
    if (g_fds[to] > 2) hio::close(g_fds[to]);
    g_fds[to] = nh;
#ifdef __3DS__
    g_packed_fds.erase(to);
    if (h == kPackedFd && g_packed_fds.count(from)) g_packed_fds[to] = g_packed_fds[from];
#endif
    return to;
}
int x_fcntl(int fd, int cmd, u32 arg) {
    if (host_fd(fd) == -1) { set_errno(kEBADF); return -1; }
    switch (cmd) {
    case 1: case 2: return 0;                                   // F_GETFD / F_SETFD
    case 3: return 2 | (nonblocking(fd) ? int(kONonblock) : 0);  // F_GETFL: O_RDWR [| O_NONBLOCK]
    case 4: {                                                   // F_SETFL
        if (host_fd(fd) == kSocketFd) net_set_nonblock(fd, (arg & kONonblock) != 0);
        std::lock_guard<std::mutex> lock(g_io_lock);
        if (g_nonblock.size() <= size_t(fd)) g_nonblock.resize(fd + 1, false);
        g_nonblock[fd] = (arg & kONonblock) != 0;
        return 0;
    }
    }
    return 0;
}

// ---- epoll (boost::asio's reactor): level-triggered EPOLLIN on pipes/files; EPOLLOUT is
// reported once after ADD/MOD (fds here are always writable), like an edge trigger. --------

int x_epoll_create(int) {
    int fd = fd_alloc(kEpollFd, "");
    std::lock_guard<std::mutex> lock(g_io_lock);
    g_epolls[fd];
    return fd;
}
int x_epoll_ctl(int epfd, int op, int fd, u32 ev) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    auto it = g_epolls.find(epfd);
    if (it == g_epolls.end()) { set_errno(kEBADF); return -1; }
    if (op == 2) { it->second.erase(fd); return 0; }  // EPOLL_CTL_DEL
    // struct epoll_event on ARM EABI: u32 events, (pad), u64 data at offset 8
    it->second[fd] = {LD32(ev), LD64(ev + 8), (LD32(ev) & 4) != 0, ~0ull};
    return 0;
}
// The watched fds are copied under the lock and polled outside it: the game's network thread
// waits here every millisecond, and holding the descriptor table's lock through its socket
// polls (WSAPoll is slow on Windows) made every file call of every other thread wait (0.1 ms a
// call: 2 s of the campaign's first load).
int x_epoll_wait(int epfd, u32 events, int max, int timeout_ms) {
    int64_t deadline = timeout_ms < 0 ? INT64_MAX : os::monotonic_ns() + int64_t(timeout_ms) * 1000000;
    struct Watched {
        int fd, h;
        EpollEntry e;
    };
    std::vector<Watched> watched;
    for (;;) {
        watched.clear();
        {
            std::lock_guard<std::mutex> lock(g_io_lock);
            auto it = g_epolls.find(epfd);
            if (it == g_epolls.end()) { set_errno(kEBADF); return -1; }
            for (const auto &[fd, e] : it->second) watched.push_back({fd, size_t(fd) < g_fds.size() ? g_fds[fd] : -1, e});
        }
        int n = 0;
        std::vector<int> out_reported;  // fds whose one EPOLLOUT after an add goes out now
        std::vector<std::pair<int, uint64_t>> edge_reported;  // EPOLLET pipes reported now: their write count
        for (const Watched &w : watched) {
            if (n >= max) break;
            u32 got = 0;
            if (w.h == kSocketFd) {
                got = net_poll(w.fd, w.e.events & 5) & w.e.events;  // EPOLLIN | EPOLLOUT
            } else {
                if ((w.e.events & 1) && w.h >= 0 && readable(w.h)) got |= 1;  // EPOLLIN
#ifdef NOVA3_PIPE_EDGES
                // EPOLLET (boost::asio's interrupter pipe, which it never drains): readable is
                // reported once per write, not as long as data is there (the reactor spun at full
                // speed on its core)
                if ((got & 1) && (w.e.events & 0x80000000u) && hio::is_pipe(w.h)) {
                    const uint64_t writes = hio::pipe_writes(w.h);
                    if (writes == w.e.seen_writes) got &= ~1u;
                    else edge_reported.push_back({w.fd, writes});
                }
#endif
                if (w.e.out_pending) {                                          // EPOLLOUT (once after an add)
                    got |= 4;
                    out_reported.push_back(w.fd);
                }
            }
            if (!got) continue;
            ST32(events + 16 * n, got);
            ST64(events + 16 * n + 8, w.e.data);
            n++;
        }
        if (!out_reported.empty() || !edge_reported.empty()) {
            std::lock_guard<std::mutex> lock(g_io_lock);
            if (auto it = g_epolls.find(epfd); it != g_epolls.end()) {
                for (int fd : out_reported)
                    if (auto e = it->second.find(fd); e != it->second.end()) e->second.out_pending = false;
                for (auto [fd, writes] : edge_reported)
                    if (auto e = it->second.find(fd); e != it->second.end()) e->second.seen_writes = writes;
            }
        }
        if (n || os::monotonic_ns() >= deadline) return n;
        os::sleep_ns(1000000);
    }
}
int x_ioctl(int fd, u32 req, u32 arg) {
    if (host_fd(fd) == kSocketFd) return net_ioctl(fd, req, arg);
    set_errno(25);  // ENOTTY
    return -1;
}
int x_pipe(u32 fds) {
    int h[2];
    if (hio::pipe(h) != 0) { set_host_errno(); return -1; }
    ST32(fds, u32(fd_alloc(h[0], "")));
    ST32(fds + 4, u32(fd_alloc(h[1], "")));
    return 0;
}
int x_mkstemp(u32 tmpl) {
    char *t = ptr(tmpl);
    size_t n = std::strlen(t);
    if (n < 6 || std::strcmp(t + n - 6, "XXXXXX")) { set_errno(kEINVAL); return -1; }
    for (int attempt = 0; attempt < 100; attempt++) {
        for (size_t i = n - 6; i < n; i++) t[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[std::rand() % 36];
        int fd = x_open(nullptr, tmpl, 0x2 | 0x40 | 0x80, 0600);
        if (fd >= 0) return fd;
    }
    return -1;
}
int x_statfs(u32 path, u32 buf) {
    (void)path;
    uint64_t avail = 0, total = 0;
    os::disk_space(g_cfg.data_dir.c_str(), avail, total);
    std::memset(ptr(buf), 0, 84);
    ST32(buf, 0xef53);   // ext4 magic
    ST32(buf + 4, 4096);
    ST64(buf + 8, total / 4096);
    ST64(buf + 16, avail / 4096);
    ST64(buf + 24, avail / 4096);
    ST64(buf + 32, 1u << 20);
    ST64(buf + 40, 1u << 19);
    ST32(buf + 56, 255);
    ST32(buf + 60, 4096);
    return 0;
}

// ---- directories ------------------------------------------------------------------------
struct Dir {
    std::vector<std::pair<std::string, bool>> entries;
    size_t pos = 0;
    u32 dirent = 0;
};
std::vector<Dir *> g_dirs;

u32 x_opendir(u32 path) {
    std::string hp = host_path(str(path));
    std::error_code ec;
    if (!fs::is_directory(hp, ec)) { set_errno(kENOENT); return 0; }
    auto *d = new Dir;
    d->entries = {{".", true}, {"..", true}};
    for (auto &e : fs::directory_iterator(hp, ec)) d->entries.push_back({e.path().filename().string(), e.is_directory()});
#ifdef __3DS__
    if (!g_packed.empty() && hp == g_cfg.obb_dir)  // packed files under their own names
        for (auto &[name, is_dir] : d->entries)
            if (!is_dir && name.size() > 2 && lower(name.substr(name.size() - 2)) == ".z" && g_packed.count(lower(name.substr(0, name.size() - 2))))
                name.resize(name.size() - 2);
#endif
    d->dirent = gcalloc(1, 280);
    u32 handle = gcalloc(1, 8);
    std::lock_guard<std::mutex> lock(g_io_lock);
    ST32(handle, u32(g_dirs.size()));
    g_dirs.push_back(d);
    return handle;
}
Dir *dir_of(u32 h) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    u32 i = LD32(h);
    return i < g_dirs.size() ? g_dirs[i] : nullptr;
}
u32 x_readdir(u32 h) {
    Dir *d = dir_of(h);
    if (!d || d->pos >= d->entries.size()) return 0;
    auto &[name, is_dir] = d->entries[d->pos++];
    // struct dirent: u64 d_ino, s64 d_off, u16 d_reclen, u8 d_type, char d_name[256]
    std::memset(ptr(d->dirent), 0, 280);
    ST64(d->dirent, d->pos);
    ST64(d->dirent + 8, d->pos);
    ST16(d->dirent + 16, 280);
    ST8(d->dirent + 18, is_dir ? 4 : 8);
    std::strncpy(ptr(d->dirent + 19), name.c_str(), 255);
    return d->dirent;
}
int x_closedir(u32 h) {
    Dir *d = dir_of(h);
    if (!d) return -1;
    gfree(d->dirent);
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        g_dirs[LD32(h)] = nullptr;
    }
    delete d;
    gfree(h);
    return 0;
}
u32 x_basename(u32 path) {
    static RT_THREAD_LOCAL(u32, buf_slots);
    u32 &buf = RT_TLS(buf_slots);
    if (!buf) buf = gmalloc(4096);
    std::string p = path ? str(path) : "";
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t slash = p.find_last_of('/');
    std::string b = p.empty() ? "." : slash == std::string::npos || p == "/" ? p : p.substr(slash + 1);
    std::strncpy(ptr(buf), b.c_str(), 4095);
    return buf;
}

// ---- stdio ----------------------------------------------------------------------------------
// bionic __sFILE (84 bytes): _flags (short) @12, _file (short) @14, _cookie @28.
constexpr u32 kFileSize = 84;
constexpr uint16_t kSEOF = 0x20, kSERR = 0x40;
std::vector<FILE *> g_files;  // index stored in _cookie

// Files opened for reading only: a block cache in front of the host file. The game reads its
// archives with a seek before almost every read, most of them small (93% under 4 KB) and many
// near the last one: on the Switch every refill of a stdio buffer is a request to the file
// system (~0.2 ms), and a seek drops the buffer (with a 64 KB buffer each small read cost a
// 64 KB transfer). Here a seek only moves the position, reads come from 16 KB blocks (the 16
// most recent kept per file), and reads of a block or more go to the file directly. Simulated
// on a recorded chapter's reads, it halves the time of the C library's 1 KB buffers. Other
// stdio calls on the file see the host FILE at the guest's position (hfile).
#ifdef __3DS__
// The archives' mount: at boot the game opens every zip .gla and reads 1 KB at each member's local
// header (about 4,400 reads spread over 1.3 GB, each a 16 KB block from the SD card: 10 s of the
// boot in Azahar). nova3_data/mount.idx (tools/3ds_slimdata.py index) holds those bytes, [h, h +
// 1 KB) for every local header h (merged where they overlap), with each archive's size. An
// archive's part is read in one request when the game first opens the archive and its size is
// checked; the first read it covers still goes to the file and is compared with it; once they
// agree, small reads inside its ranges are served from it. The parts are freed when the boot is
// over (io_boot_done). Format: "N3MI", version 1, count, directory size (u32 each); the directory:
// per archive u16 name length, the name, u32 size, u32 offset of its part, u32 part size; a part:
// u32 n, n x (u32 offset, u32 length), then the n ranges' bytes.
struct IndexPart {
    struct Range { uint32_t offset, length; const uint8_t *bytes; };
    uint32_t size = 0, at = 0, bytes = 0;  // the archive's size; where its part is in mount.idx
    enum { kUnloaded, kUnchecked, kChecked, kOff } state = kUnloaded;
    std::vector<uint8_t> data;             // the part as read
    std::vector<Range> ranges;             // by offset, into data
    // the range holding [pos, pos + n) (clipped to the archive's end), or nullptr
    const Range *find(int64_t pos, uint64_t n) const {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), pos, [](int64_t p, const Range &r) { return p < r.offset; });
        if (it == ranges.begin() || pos >= int64_t(size)) return nullptr;
        --it;
        return std::min<int64_t>(pos + int64_t(n), size) <= int64_t(it->offset) + it->length ? &*it : nullptr;
    }
};
std::mutex g_index_lock;
std::unordered_map<std::string, IndexPart> g_index;  // by the archive's lower-case host path
FILE *g_index_file;                                   // open until the boot is over
uint64_t g_index_reads;                               // reads it served

void load_index() {
    if (!g_data_listed || !g_data_names.count("mount.idx")) return;
    FILE *f = std::fopen((g_cfg.obb_dir + "/mount.idx").c_str(), "rb");
    if (!f) return;
    std::setvbuf(f, nullptr, _IONBF, 0);  // (each read one request of its own size)
    uint32_t head[4];
    std::vector<uint8_t> dir;
    if (std::fread(head, 4, 4, f) == 4 && !std::memcmp(head, "N3MI", 4) && head[1] == 1 && head[3] < (1u << 20)) {
        dir.resize(head[3]);
        if (std::fread(dir.data(), 1, dir.size(), f) != dir.size()) dir.clear();
    }
    size_t at = 0;
    for (uint32_t i = 0; i < head[2] && at + 2 <= dir.size(); i++) {
        uint16_t len;
        std::memcpy(&len, &dir[at], 2);
        if (at + 2 + len + 12 > dir.size()) break;
        IndexPart &p = g_index[lower(g_cfg.obb_dir + "/" + std::string(reinterpret_cast<const char *>(&dir[at + 2]), len))];
        std::memcpy(&p.size, &dir[at + 2 + len], 12);  // size, at, bytes
        at += 2 + len + 12;
    }
    if (g_index.empty()) { std::fclose(f); return; }
    g_index_file = f;
    log("files: mount index for %zu archives", g_index.size());
}
// the index part of a game file the guest opens (loaded at its first open), or nullptr
IndexPart *index_part(const std::string &host, FILE *f) {
    std::lock_guard<std::mutex> lock(g_index_lock);
    if (!g_index_file) return nullptr;
    auto it = g_index.find(lower(host));
    if (it == g_index.end()) return nullptr;
    IndexPart &p = it->second;
    if (p.state == IndexPart::kUnloaded) {
        p.state = IndexPart::kOff;
        hio::stat_t st;
        if (hio::fstat(hio::fileno(f), &st) != 0 || uint64_t(st.st_size) != p.size) {
            log("files: %s is not the archive mount.idx describes (%llu bytes, not %u)", host.c_str(),
                (unsigned long long)st.st_size, p.size);
            return nullptr;
        }
        p.data.resize(p.bytes);
        bool ok;
        {
            HostRequest req(kIoRead, g_host_reads, g_host_read_ticks);
            ok = std::fseek(g_index_file, long(p.at), SEEK_SET) == 0 && std::fread(p.data.data(), 1, p.bytes, g_index_file) == p.bytes;
        }
        g_host_read_bytes += p.bytes;
        uint32_t n = 0;
        if (ok && p.bytes >= 4) std::memcpy(&n, p.data.data(), 4);
        size_t table = 4, bytes = 4 + size_t(n) * 8;
        for (uint32_t i = 0; ok && i < n && bytes <= p.bytes; i++, table += 8) {
            IndexPart::Range r;
            std::memcpy(&r.offset, &p.data[table], 4);
            std::memcpy(&r.length, &p.data[table + 4], 4);
            r.bytes = p.data.data() + bytes;
            bytes += r.length;
            p.ranges.push_back(r);
        }
        if (!ok || bytes != p.bytes) {
            log("files: mount.idx is damaged (%s)", host.c_str());
            p.data = {}, p.ranges = {};
            return nullptr;
        }
        p.state = IndexPart::kUnchecked;
    }
    return p.state == IndexPart::kOff ? nullptr : &p;
}
// a small read from the index, if it holds it (the part checked)
bool index_read(IndexPart &p, int64_t pos, uint8_t *dst, uint64_t want, uint64_t &got, bool &eof) {
    std::lock_guard<std::mutex> lock(g_index_lock);
    if (p.state != IndexPart::kChecked) return false;
    const IndexPart::Range *r = p.find(pos, want);
    if (!r) return false;
    got = std::min<uint64_t>(want, uint64_t(p.size - pos));
    std::memcpy(dst, r->bytes + (pos - r->offset), size_t(got));
    eof = pos + int64_t(want) > int64_t(p.size);
    g_index_reads++;
    return true;
}
// a read made from the file: the first one the index covers decides whether the index is used
void index_check(IndexPart &p, int64_t pos, const uint8_t *data, uint64_t got) {
    std::lock_guard<std::mutex> lock(g_index_lock);
    if (p.state != IndexPart::kUnchecked || !got) return;
    const IndexPart::Range *r = p.find(pos, got);
    if (!r) return;
    if (!std::memcmp(r->bytes + (pos - r->offset), data, size_t(got))) {
        p.state = IndexPart::kChecked;
        return;
    }
    log("files: mount.idx differs from the archive at offset %lld: not used for it", (long long)pos);
    p.state = IndexPart::kOff;
    p.data = {}, p.ranges = {};
}
#endif

// Blocks held by all the read caches together, and their limit: the game keeps ~60 archives open,
// at 16 blocks each 15 MB, more than the GameCube's host has; there a file past the limit reuses
// its own oldest block.
std::atomic<size_t> g_cache_blocks{0};
#ifdef __GAMECUBE__
constexpr size_t kCacheBlocksTotal = 24;  // 384 KB
#else
constexpr size_t kCacheBlocksTotal = SIZE_MAX;
#endif
struct ReadCache {
    static constexpr int64_t kBlock = 16 * 1024;
    static constexpr size_t kBlocks = 16;
    ~ReadCache() { g_cache_blocks -= blocks.size(); }
    std::mutex lock;
    int64_t pos = 0;
    bool host_owned = false;  // another stdio call used the FILE since: its position is the truth
    std::list<std::pair<int64_t, std::vector<uint8_t>>> blocks;  // block index, data (a short last block); recent first
    std::string park_path;    // a game file kept open when the guest closes it (below), or empty
#ifdef __3DS__
    IndexPart *index = nullptr;  // its part of the mount index (above)
    int64_t size = -1;           // the file's size, once asked (game files never change)
    Packed *packed = nullptr;    // a packed file (above): read through its unpacked blocks, no blocks here
#endif
};
std::vector<std::unique_ptr<ReadCache>> g_caches;  // parallel to g_files

#if defined(__3DS__) || defined(__SWITCH__) || defined(__GAMECUBE__)
// The game reads its archives (the OBBs' contents) by opening the file, seeking, reading and
// closing it again for every piece: a level load opens the two OBBs ~1,600 times. On the
// consoles each open and close is a request to the file system service, and each open started
// with a cold cache. So a read-only game file the guest closes stays open, with its block
// cache, and the next fopen of the same path takes it back. Game files never change (saves
// and scratch files are not kept).
struct Parked {
    FILE *f;
    std::unique_ptr<ReadCache> cache;
};
std::list<Parked> g_parked;  // most recent first; under g_io_lock
constexpr size_t kParked = 4;
bool parkable(const std::string &host) {
    const std::string &data = g_cfg.obb_dir;
    return !data.empty() && host.compare(0, data.size() + 1, data + "/") == 0;
}
// a parked file for this path (its cache's position back at the start), or nullptr
FILE *unpark(const std::string &host, std::unique_ptr<ReadCache> &cache) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    for (auto it = g_parked.begin(); it != g_parked.end(); ++it) {
        if (it->cache->park_path != host) continue;
        FILE *f = it->f;
        cache = std::move(it->cache);
        g_parked.erase(it);
        cache->pos = 0;
        cache->host_owned = false;
        std::clearerr(f);
        return f;
    }
    return nullptr;
}
// keeps a closed game file open; the oldest parked one is closed
void park(FILE *f, std::unique_ptr<ReadCache> cache) {
    FILE *evicted = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        g_parked.push_front({f, std::move(cache)});
        if (g_parked.size() > kParked) {
            evicted = g_parked.back().f;
            g_parked.pop_back();
        }
    }
    if (evicted) std::fclose(evicted);
}
#endif

FILE *raw_file(u32 fp, ReadCache **cache = nullptr) {
    std::lock_guard<std::mutex> lock(g_io_lock);
    u32 i = fp ? LD32(fp + 28) : ~0u;
    if (i >= g_files.size() || !g_files[i]) fatal("bad guest FILE* %08x", fp);
    if (cache) *cache = i < g_caches.size() ? g_caches[i].get() : nullptr;
    return g_files[i];
}
// The host FILE, at the guest's position.
FILE *hfile(u32 fp) {
    ReadCache *c;
    FILE *f = raw_file(fp, &c);
    if (c) {
        std::lock_guard<std::mutex> lock(c->lock);
        if (!c->host_owned) {
            std::fseek(f, long(c->pos), SEEK_SET);
            c->host_owned = true;
        }
    }
    return f;
}
// A read-only file's cache, its position taken back from the FILE if another call used it.
ReadCache *cache_of(u32 fp, FILE *&f) {
    ReadCache *c;
    f = raw_file(fp, &c);
    if (c && c->host_owned) {
        std::lock_guard<std::mutex> lock(c->lock);
        c->pos = std::ftell(f);
        c->host_owned = false;
    }
    return c;
}
void sync_flags(u32 fp, FILE *f) {
    uint16_t flags = uint16_t(LD16(fp + 12) & ~(kSEOF | kSERR));
    if (std::feof(f)) flags |= kSEOF;
    if (std::ferror(f)) flags |= kSERR;
    ST16(fp + 12, flags);
}
u32 wrap_file(FILE *f, int fd, u32 at = 0) {
    u32 fp = at ? at : gcalloc(1, kFileSize);
    std::lock_guard<std::mutex> lock(g_io_lock);
    ST16(fp + 12, 0x0010);  // __SRW
    ST16(fp + 14, uint16_t(fd));
    ST32(fp + 28, u32(g_files.size()));
    g_files.push_back(f);
    return fp;
}

uint32_t init_sF(const char *) {
    // __sF[3]: stdin, stdout, stderr laid out back to back
    u32 base = gcalloc(1, kFileSize * 3);
    wrap_file(stdin, 0, base);
    wrap_file(stdout, 1, base + kFileSize);
    wrap_file(stderr, 2, base + 2 * kFileSize);
    return base;
}

std::string host_mode(const char *m) {
    std::string s;
    for (const char *p = m; *p; p++) if (*p != 'b' && *p != 'e') s += *p;
    return s + "b";
}

u32 x_fopen(u32 path, u32 mode) {
#ifdef __3DS__
    if (const char *ms = str(mode)) {  // (read only: before any string is built)
        while (*ms == 'b' || *ms == 'e') ms++;
        if (*ms == 'r' && !std::strchr(ms, '+') && quick_miss(path)) return 0;
    }
#endif
    std::string m = host_mode(str(mode));
    const bool read_only = m[0] == 'r' && m.find('+') == std::string::npos;
    std::string hp = host_path(str(path));
    if (m[0] == 'w' || m[0] == 'a') {
        std::error_code ec;
        fs::create_directories(fs::path(hp).parent_path(), ec);
    }
    if (!read_only) forget_missing(hp);
    else if (known_missing(hp)) {
        note_guest_missing(path);
        trace_file("fopen", path, hp, false);
        set_errno(kENOENT);
        return 0;
    }
    std::unique_ptr<ReadCache> cache;
#if defined(__3DS__) || defined(__SWITCH__) || defined(__GAMECUBE__)
    FILE *f = read_only && parkable(hp) ? unpark(hp, cache) : nullptr;
#ifdef __3DS__
    Packed *packed = read_only && !f ? packed_file(hp) : nullptr;  // (no request: the container stays open)
    if (packed && (f = packed_stdio(packed)) != nullptr) {
        cache = std::make_unique<ReadCache>();  // (not parked: nothing to keep)
        cache->packed = packed;
        cache->size = int64_t(packed->size);
    }
#endif
    if (!f) {
        HostRequest req(kIoOpen, g_host_opens, g_host_open_ticks);
        f = std::fopen(hp.c_str(), m.c_str());
    }
#else
    FILE *f;
    {
        HostRequest req(kIoOpen, g_host_opens, g_host_open_ticks);
        f = std::fopen(hp.c_str(), m.c_str());
    }
#endif
    if (!f && read_only && note_missing(hp)) note_guest_missing(path);  // (by errno: before anything else can change it)
    trace_file("fopen", path, hp, f != nullptr);
    if (!f) {
        set_host_errno();
        return 0;
    }
#ifdef __GAMECUBE__
    // the player's files are in memory ("ram:") and the disc's driver keeps a 64 KB block of its
    // own (runtime/gc_dvdfs.c): stdio's buffers would only take MEM1 (16 KB a file, ~25 open)
    if (!read_only) std::setvbuf(f, nullptr, _IOFBF, 4 * 1024);
    else if (!cache) std::setvbuf(f, nullptr, _IONBF, 0);
#else
    // writes (saves) are sequential: a large buffer makes them few requests
    if (!read_only) std::setvbuf(f, nullptr, _IOFBF, 64 * 1024);
#endif
#if defined(__3DS__) || defined(__SWITCH__)
    // reads too: newlib refills its 1 KB buffer once per KB of an fread, and every refill is a
    // request to the file system service (the 3DS's SD card: about 1 ms each; the Switch: ~0.2
    // ms); the read cache's 16 KB blocks then come in one request
    else if (!cache) std::setvbuf(f, nullptr, _IOFBF, size_t(ReadCache::kBlock));
#endif
#if defined(__3DS__) || defined(__SWITCH__) || defined(__GAMECUBE__)
    if (read_only && !cache && parkable(hp)) {
        cache = std::make_unique<ReadCache>();
        cache->park_path = hp;
#ifdef __3DS__
        cache->index = index_part(hp, f);
#endif
    }
#endif
#ifdef __3DS__
    const int fd = cache && cache->packed ? packed_fd(cache->packed, hp) : fd_alloc(hio::dup(hio::fileno(f)), hp);
#else
    const int fd = fd_alloc(hio::dup(hio::fileno(f)), hp);
#endif
    const u32 fp = wrap_file(f, fd);
    if (read_only) {
        std::lock_guard<std::mutex> lock(g_io_lock);
        const u32 i = LD32(fp + 28);
        if (g_caches.size() <= i) g_caches.resize(i + 1);
        g_caches[i] = cache ? std::move(cache) : std::make_unique<ReadCache>();
    }
    return fp;
}
u32 x_fdopen(int fd, u32 mode) {
    int h = host_fd(fd);
#ifdef __3DS__
    if (PackedFd d; h == kPackedFd) {  // (stdio over the packed file, from the descriptor's position)
        if (!packed_fd_of(fd, d)) return 0;
        FILE *f = packed_stdio(d.p, d.pos);
        if (!f) { set_errno(kENOMEM); return 0; }
        return wrap_file(f, fd);
    }
#endif
    if (h < 0) { set_errno(kEBADF); return 0; }
    FILE *f = hio::fdopen(hio::dup(h), host_mode(str(mode)).c_str());
    if (!f) { set_host_errno(); return 0; }
    return wrap_file(f, fd);
}
int x_fclose(u32 fp) {
    FILE *f = raw_file(fp);
    int fd = int16_t(LD16(fp + 14));
    std::unique_ptr<ReadCache> cache;
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        const u32 i = LD32(fp + 28);
        g_files[i] = nullptr;
        if (i < g_caches.size()) cache = std::move(g_caches[i]);
    }
    int r = 0;
#if defined(__3DS__) || defined(__SWITCH__) || defined(__GAMECUBE__)
    if (cache && !cache->park_path.empty()) park(f, std::move(cache));  // stays open (above)
    else
#endif
    if (f != stdin && f != stdout && f != stderr) r = std::fclose(f);
    if (fd > 2) x_close(fd);
    if (fp < data_import_address("__sF") || fp >= data_import_address("__sF") + 3 * kFileSize) gfree(fp);
    return r ? -1 : 0;
}
u32 x_freopen(u32 path, u32 mode, u32 fp) {
    FILE *f = hfile(fp);
    FILE *nf = path ? std::freopen(host_path(str(path)).c_str(), host_mode(str(mode)).c_str(), f) : f;
    if (!nf) { set_host_errno(); return 0; }
    std::lock_guard<std::mutex> lock(g_io_lock);
    const u32 i = LD32(fp + 28);
    g_files[i] = nf;
    if (i < g_caches.size()) g_caches[i].reset();  // (another file, any mode)
    return fp;
}
// guest FILE flags of a cached file (its end of file is the cache's, not the host FILE's)
void set_eof(u32 fp, bool eof) {
    ST16(fp + 12, uint16_t((LD16(fp + 12) & ~kSEOF) | (eof ? kSEOF : 0)));
}
// A host read at `pos`. On the 3DS straight from the file's descriptor: newlib's fseek refills its
// buffer at once from the 1 KB boundary below the position (it takes 1 KB as an SD file's block
// size), so every read at an unaligned position was a 16 KB request more, before the read itself.
// The FILE's own buffer serves only the other stdio calls, which seek first (hfile; newlib keeps
// the offset of what it buffered itself).
size_t host_read(FILE *f, int64_t pos, uint8_t *dst, size_t n) {
#ifdef __3DS__
    return fd_read_at(hio::fileno(f), pos, dst, n);
#else
    std::fseek(f, long(pos), SEEK_SET);
    return std::fread(dst, 1, n, f);
#endif
}
u32 cached_read(ReadCache &c, FILE *f, uint8_t *dst, uint64_t want, bool &eof) {
    using Block = std::pair<int64_t, std::vector<uint8_t>>;
#ifdef __3DS__
    if (c.packed) {  // a packed file: its own blocks
        const size_t got = packed_read(*c.packed, c.pos, dst, size_t(want));
        c.pos += int64_t(got);
        eof = got < want;
        return u32(got);
    }
    const int64_t start = c.pos;
    if (c.index && int64_t(want) < ReadCache::kBlock) {  // the archives' mount (above)
        uint64_t got;
        if (index_read(*c.index, c.pos, dst, want, got, eof)) {
            c.pos += int64_t(got);
            return u32(got);
        }
    }
#endif
    if (int64_t(want) >= ReadCache::kBlock) {  // large: straight from the file
        size_t got;
        {
            HostRequest req(kIoRead, g_host_reads, g_host_read_ticks);
            got = host_read(f, c.pos, dst, size_t(want));
        }
        g_host_read_bytes += got;
        c.pos += int64_t(got);
        eof = got < want;
        return u32(got);
    }
    uint64_t done = 0;
    while (done < want) {
        const int64_t index = c.pos / ReadCache::kBlock;
        auto it = std::find_if(c.blocks.begin(), c.blocks.end(), [index](const Block &b) { return b.first == index; });
        if (it == c.blocks.end()) {
            if (c.blocks.empty() || (c.blocks.size() < ReadCache::kBlocks && g_cache_blocks < kCacheBlocksTotal)) {
                c.blocks.emplace_front();
                g_cache_blocks++;
            } else {
                c.blocks.splice(c.blocks.begin(), c.blocks, std::prev(c.blocks.end()));  // (the oldest one's memory)
            }
            Block &b = c.blocks.front();
            b.first = index;
            b.second.resize(size_t(ReadCache::kBlock));
            {
                HostRequest req(kIoRead, g_host_reads, g_host_read_ticks);
                b.second.resize(host_read(f, index * ReadCache::kBlock, b.second.data(), b.second.size()));
            }
            g_host_read_bytes += b.second.size();
        } else if (it != c.blocks.begin()) {
            c.blocks.splice(c.blocks.begin(), c.blocks, it);
        }
        const std::vector<uint8_t> &data = c.blocks.front().second;
        const int64_t at = c.pos - index * ReadCache::kBlock;
        if (at >= int64_t(data.size())) {  // past the end of the file
            eof = true;
            break;
        }
        const uint64_t take = std::min<uint64_t>(want - done, uint64_t(int64_t(data.size()) - at));
        std::memcpy(dst + done, data.data() + at, size_t(take));
        done += take;
        c.pos += int64_t(take);
    }
#ifdef __3DS__
    if (c.index) index_check(*c.index, start, dst, done);
#endif
    return u32(done);
}
u32 x_fread(u32 buf, u32 size, u32 n, u32 fp) {
    FILE *f;
    if (ReadCache *c = cache_of(fp, f)) {
        if (!size || !n) return 0;
        std::lock_guard<std::mutex> lock(c->lock);
        bool eof = false;
        const int64_t at = c->pos;
        const u32 got = cached_read(*c, f, reinterpret_cast<uint8_t *>(ptr(buf)), uint64_t(size) * n, eof);
        set_eof(fp, eof);
        if (tracing_reads()) trace_read(int16_t(LD16(fp + 14)), at, got);
        return got / size;
    }
    const long at = tracing_reads() ? std::ftell(f) : 0;
    u32 r = u32(std::fread(ptr(buf), size, n, f));
    sync_flags(fp, f);
    if (tracing_reads()) trace_read(int16_t(LD16(fp + 14)), at, uint64_t(r) * size);
    return r;
}
u32 x_fwrite(u32 buf, u32 size, u32 n, u32 fp) {
    FILE *f = hfile(fp);
    u32 r = u32(std::fwrite(ptr(buf), size, n, f));
    sync_flags(fp, f);
    return r;
}
u32 x_fgets(u32 buf, int n, u32 fp) {
    FILE *f = hfile(fp);
    char *r = std::fgets(ptr(buf), n, f);
    sync_flags(fp, f);
    return r ? buf : 0;
}
int x_fgetc(u32 fp) { FILE *f = hfile(fp); int c = std::fgetc(f); sync_flags(fp, f); return c; }
int x_ungetc(int c, u32 fp) { FILE *f = hfile(fp); int r = std::ungetc(c, f); sync_flags(fp, f); return r; }
int x_fputc(int c, u32 fp) { FILE *f = hfile(fp); int r = std::fputc(c, f); sync_flags(fp, f); return r; }
int x_putchar(int c) { return std::putchar(c); }
int x_fputs(u32 s, u32 fp) { FILE *f = hfile(fp); int r = std::fputs(str(s), f); sync_flags(fp, f); return r < 0 ? -1 : 0; }
int x_puts(u32 s) { return std::puts(str(s)) < 0 ? -1 : 0; }
int x_fseek(u32 fp, int32_t off, int whence) {
    FILE *f;
    if (ReadCache *c = cache_of(fp, f)) {
        std::lock_guard<std::mutex> lock(c->lock);
        int64_t base = c->pos;
        if (whence == SEEK_END) {
#ifdef __3DS__
            // (a game file's size asked once: newlib's own SEEK_END is a size request and a read)
            if (hio::stat_t st; c->size < 0 && !c->park_path.empty() && hio::fstat(hio::fileno(f), &st) == 0)
                c->size = int64_t(st.st_size);
            if (c->size >= 0) base = c->size;
            else
#endif
            {
                std::fseek(f, 0, SEEK_END);
                base = std::ftell(f);
            }
        } else if (whence == SEEK_SET) {
            base = 0;
        }
        if (base + off < 0) { set_errno(kEINVAL); return -1; }
        c->pos = base + off;
        set_eof(fp, false);
        return 0;
    }
    f = hfile(fp);
    int r = std::fseek(f, off, whence);
    sync_flags(fp, f);
    if (r) set_host_errno();
    return r ? -1 : 0;
}
int32_t x_ftell(u32 fp) {
    FILE *f;
    if (ReadCache *c = cache_of(fp, f)) {
        std::lock_guard<std::mutex> lock(c->lock);
        return int32_t(c->pos);
    }
    return int32_t(std::ftell(f));
}
int x_fflush(u32 fp) { return std::fflush(fp ? hfile(fp) : nullptr) ? -1 : 0; }
int x_setvbuf(u32, u32, int, u32) { return 0; }
void x_setbuf(u32, u32) {}
void x_clearerr(u32 fp) { FILE *f = hfile(fp); std::clearerr(f); sync_flags(fp, f); }
void x_flockfile(u32 fp) { hio::lock_file(hfile(fp)); }
void x_funlockfile(u32 fp) { hio::unlock_file(hfile(fp)); }
void x_perror(u32 s) { std::fprintf(stderr, "%s: errno %d\n", s ? str(s) : "", get_errno()); }
u32 x_tmpfile() {
    FILE *f = std::tmpfile();
    if (!f) { set_errno(kEIO); return 0; }
    return wrap_file(f, -1);
}
u32 x_tmpnam(u32 buf) {
    static u32 sbuf;
    if (!buf) buf = sbuf ? sbuf : (sbuf = gmalloc(64));
    static int counter;
    std::snprintf(ptr(buf), 64, "/tmp/tmp%06d", ++counter);
    return buf;
}
u32 x_getwc(u32 fp) { int c = x_fgetc(fp); return c < 0 ? 0xffffffffu : u32(c); }
u32 x_putwc(u32 c, u32 fp) { return x_fputc(int(c & 0xff), fp) < 0 ? 0xffffffffu : c; }
u32 x_ungetwc(u32 c, u32 fp) { return x_ungetc(int(c & 0xff), fp) < 0 ? 0xffffffffu : c; }

}  // namespace

// ---- fd helpers for the socket layer (net.cpp) ------------------------------------------------
int io_alloc_socket_fd() { return fd_alloc(kSocketFd, "socket"); }
bool io_is_socket(int fd) { return host_fd(fd) == kSocketFd; }
int io_poll_fd(int fd, int events) {  // POLLIN 1 / POLLOUT 4 readiness of a non-socket fd
    int h = host_fd(fd);
    if (h == -1) return 0x20;  // POLLNVAL
    int got = 0;
    if ((events & 1) && h >= 0 && readable(h)) got |= 1;
    if (events & 4) got |= 4;
    return got;
}

// ---- printf --------------------------------------------------------------------------------
// Formats with guest arguments: each conversion is rebuilt as a host spec and printed
// with the host snprintf (32-bit `long`, 32-bit pointers, guest strings).
std::string guest_format(const char *fmt, Args &args) {
    std::string out;
    char spec[64], buf[512];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out += *p; continue; }
        const char *start = p++;
        if (*p == '%') { out += '%'; continue; }
        std::string flags;
        while (*p && std::strchr("-+ #0'", *p)) flags += *p++;
        std::string width, prec;
        if (*p == '*') { width = std::to_string(int32_t(args.word())); p++; }
        else while (*p >= '0' && *p <= '9') width += *p++;
        bool has_prec = false;
        if (*p == '.') {
            has_prec = true;
            p++;
            if (*p == '*') { prec = std::to_string(int32_t(args.word())); p++; }
            else while (*p >= '0' && *p <= '9') prec += *p++;
        }
        std::string len;
        while (*p && std::strchr("hlLqjzt", *p)) len += *p++;
        char conv = *p;
        if (!conv) break;
        std::string base = "%" + flags + width + (has_prec ? "." + prec : "");
        bool wide = len == "ll" || len == "q" || len == "j" || len == "L";
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = wide ? int64_t(args.dword()) : int64_t(int32_t(args.word()));
            if (len == "h") v = int16_t(v);
            if (len == "hh") v = int8_t(v);
            std::snprintf(spec, sizeof spec, "%sll%c", base.c_str(), conv);
            std::snprintf(buf, sizeof buf, spec, (long long)v);
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = wide ? args.dword() : uint64_t(args.word());
            if (len == "h") v = uint16_t(v);
            if (len == "hh") v = uint8_t(v);
            std::snprintf(spec, sizeof spec, "%sll%c", base.c_str(), conv);
            std::snprintf(buf, sizeof buf, spec, (unsigned long long)v);
            out += buf;
            break;
        }
        case 'p': {
            std::snprintf(spec, sizeof spec, "%s#x", ("%" + flags + width).c_str());
            std::snprintf(buf, sizeof buf, spec, args.word());
            out += buf;
            break;
        }
        case 'c': {
            std::snprintf(spec, sizeof spec, "%sc", base.c_str());
            std::snprintf(buf, sizeof buf, spec, int(args.word()));
            out += buf;
            break;
        }
        case 's': {
            u32 s = args.word();
            const char *hs = s ? str(s) : "(null)";
            std::string tmp;
            if (len == "l" && s) {  // 32-bit wide string -> narrow
                for (u32 a = s; LD32(a); a += 4) tmp += char(LD32(a));
                hs = tmp.c_str();
            }
            std::snprintf(spec, sizeof spec, "%ss", base.c_str());
            int n = std::snprintf(nullptr, 0, spec, hs);
            std::string r(size_t(n), '\0');
            std::snprintf(r.data(), r.size() + 1, spec, hs);
            out += r;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = args.dword();
            double d;
            std::memcpy(&d, &bits, 8);
            std::snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
            int n = std::snprintf(nullptr, 0, spec, d);
            std::string r(size_t(n), '\0');
            std::snprintf(r.data(), r.size() + 1, spec, d);
            out += r;
            break;
        }
        case 'n': {
            u32 dst = args.word();
            if (dst) ST32(dst, u32(out.size()));
            break;
        }
        default:
            out.append(start, p + 1);
        }
    }
    return out;
}

namespace {

void out_file(u32 fp, const std::string &s) {
    FILE *f = hfile(fp);
    std::fwrite(s.data(), 1, s.size(), f);
    sync_flags(fp, f);
}
int store_n(u32 dst, u32 n, const std::string &s) {
    if (n) {
        size_t k = std::min<size_t>(s.size(), n - 1);
        std::memcpy(ptr(dst), s.data(), k);
        ptr(dst)[k] = 0;
    }
    return int(s.size());
}

void i_printf(Cpu *c) { Args a(c, 1); std::string s = guest_format(str(c->r[0]), a); std::fwrite(s.data(), 1, s.size(), stdout); c->r[0] = u32(s.size()); }
void i_fprintf(Cpu *c) { Args a(c, 2); std::string s = guest_format(str(c->r[1]), a); out_file(c->r[0], s); c->r[0] = u32(s.size()); }
void i_vfprintf(Cpu *c) { Args a = Args::va_list(c, c->r[2]); std::string s = guest_format(str(c->r[1]), a); out_file(c->r[0], s); c->r[0] = u32(s.size()); }
void i_sprintf(Cpu *c) { Args a(c, 2); std::string s = guest_format(str(c->r[1]), a); c->r[0] = u32(store_n(c->r[0], u32(s.size() + 1), s)); }
void i_vsprintf(Cpu *c) { Args a = Args::va_list(c, c->r[2]); std::string s = guest_format(str(c->r[1]), a); c->r[0] = u32(store_n(c->r[0], u32(s.size() + 1), s)); }
void i_snprintf(Cpu *c) { Args a(c, 3); std::string s = guest_format(str(c->r[2]), a); c->r[0] = u32(store_n(c->r[0], c->r[1], s)); }
void i_vsnprintf(Cpu *c) { Args a = Args::va_list(c, c->r[3]); std::string s = guest_format(str(c->r[2]), a); c->r[0] = u32(store_n(c->r[0], c->r[1], s)); }

// ---- scanf -------------------------------------------------------------------------------------
// Each conversion is run separately with the host sscanf ("<literal>%<conv>%n") so guest
// destination pointers can be filled with the correct guest-side widths. A string conversion
// stores at most the rest of the input (or its width): the buffer for it is sized by that, once
// per thread (a 64 KB buffer allocated and cleared per call was 8% of the 3DS's boot: the game
// parses its menus' text with sscanf).
int guest_scan(const char *input, const char *fmt, Args &args) {
    int assigned = 0;
    size_t pos = 0;
    const char *p = fmt;
    static RT_THREAD_LOCAL(std::vector<char>, sbuf_slots);
    std::vector<char> &sbuf = RT_TLS(sbuf_slots);
    while (*p) {
        std::string piece;
        while (*p && *p != '%') piece += *p++;
        if (!*p) {
            int n = 0;
            std::sscanf(input + pos, (piece + "%n").c_str(), &n);
            break;
        }
        if (p[1] == '%') { piece += "%%"; p += 2; int n = 0; std::sscanf(input + pos, (piece + "%n").c_str(), &n); if (!n) break; pos += size_t(n); continue; }
        const char *conv_start = p++;
        bool suppress = *p == '*';
        if (suppress) p++;
        std::string width;
        while (*p >= '0' && *p <= '9') width += *p++;
        std::string len;
        while (*p && std::strchr("hlLqjzt", *p)) len += *p++;
        std::string set;
        char conv = *p++;
        if (conv == '[') {
            set = "[";
            if (*p == '^') set += *p++;
            if (*p == ']') set += *p++;
            while (*p && *p != ']') set += *p++;
            if (*p) set += *p++;
        }
        (void)conv_start;
        if (input[pos] == 0 && conv != 'n') return assigned ? assigned : -1;
        int n = -1, got = 0;
        std::string host_conv = "%" + std::string(suppress ? "*" : "") + width;
        u32 dst = suppress || conv == 'n' ? 0 : args.word();
        if (conv == 'n') { if (!suppress) ST32(args.word(), u32(pos)); continue; }
        bool wide = len == "ll" || len == "q" || len == "j" || len == "L";
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            long long v = 0;
            got = std::sscanf(input + pos, (piece + host_conv + "ll" + conv + "%n").c_str(), &v, &n);
            if (got == 1 && dst) {
                if (wide) ST64(dst, uint64_t(v));
                else if (len == "h") ST16(dst, u32(v));
                else if (len == "hh") ST8(dst, uint8_t(v));
                else ST32(dst, u32(v));
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            double v = 0;
            got = std::sscanf(input + pos, (piece + host_conv + "l" + conv + "%n").c_str(), &v, &n);
            if (got == 1 && dst) {
                if (len == "l" || len == "L") { uint64_t b; std::memcpy(&b, &v, 8); ST64(dst, b); }
                else { float f = float(v); u32 b; std::memcpy(&b, &f, 4); ST32(dst, b); }
            }
            break;
        }
        case 's': case 'c': case '[': {
            std::string c = conv == '[' ? set : std::string(1, conv);
            if (width.empty() && conv == 'c') host_conv += "1";
            const size_t need = std::max<size_t>(std::strlen(input + pos), width.empty() ? 1 : size_t(std::stoi(width))) + 1;
            if (sbuf.size() < need) sbuf.resize(need);
            sbuf[0] = 0;
            got = std::sscanf(input + pos, (piece + host_conv + c + "%n").c_str(), sbuf.data(), &n);
            if (got == 1 && dst) {
                size_t k = conv == 'c' ? size_t(width.empty() ? 1 : std::stoi(width)) : std::strlen(sbuf.data()) + 1;
                std::memcpy(ptr(dst), sbuf.data(), k);
            }
            break;
        }
        default:
            return assigned;
        }
        if (suppress) {
            int m = 0;
            std::string f = piece + host_conv + (conv == '[' ? set : std::string(1, conv)) + "%n";
            std::sscanf(input + pos, f.c_str(), &m);
            if (!m) return assigned;
            pos += size_t(m);
            continue;
        }
        if (got != 1 || n < 0) return assigned ? assigned : (got == EOF ? -1 : 0);
        assigned++;
        pos += size_t(n);
    }
    return assigned;
}

void i_sscanf(Cpu *c) { Args a(c, 2); c->r[0] = u32(guest_scan(str(c->r[0]), str(c->r[1]), a)); }
void i_fscanf(Cpu *c) {
    // Read one line and scan it (the game uses fscanf on line-oriented config files).
    FILE *f = hfile(c->r[0]);
    std::string line;
    int ch;
    while ((ch = std::fgetc(f)) != EOF) { line += char(ch); if (ch == '\n') break; }
    sync_flags(c->r[0], f);
    if (line.empty()) { c->r[0] = u32(-1); return; }
    Args a(c, 2);
    c->r[0] = u32(guest_scan(line.c_str(), str(c->r[1]), a));
}

}  // namespace

RT_IMPORT_RAW(open, [](Cpu *c) { c->r[0] = u32(x_open(c, c->r[0], int(c->r[1]), c->r[2])); })
RT_IMPORT(close, x_close)
RT_IMPORT(read, x_read)
RT_IMPORT(write, x_write)
RT_IMPORT(writev, x_writev)
RT_IMPORT(lseek, x_lseek)
RT_IMPORT(pread, x_pread)
RT_IMPORT(fstat, x_fstat)
RT_IMPORT(stat, x_stat)
RT_IMPORT(access, x_access)
RT_IMPORT(mkdir, x_mkdir)
RT_IMPORT(unlink, x_unlink)
RT_IMPORT(remove, x_remove)
RT_IMPORT(rename, x_rename)
RT_IMPORT(chmod, x_chmod)
RT_IMPORT(umask, x_umask)
RT_IMPORT(chdir, x_chdir)
RT_IMPORT(dup2, x_dup2)
RT_IMPORT_RAW(fcntl, [](Cpu *c) { c->r[0] = u32(x_fcntl(int(c->r[0]), int(c->r[1]), c->r[2])); })
RT_IMPORT_RAW(ioctl, [](Cpu *c) { c->r[0] = u32(x_ioctl(int(c->r[0]), c->r[1], c->r[2])); })
RT_IMPORT(pipe, x_pipe)
RT_IMPORT(epoll_create, x_epoll_create)
RT_IMPORT(epoll_ctl, x_epoll_ctl)
RT_IMPORT(epoll_wait, x_epoll_wait)
RT_IMPORT(mkstemp, x_mkstemp)
RT_IMPORT(statfs, x_statfs)
RT_IMPORT(opendir, x_opendir)
RT_IMPORT(readdir, x_readdir)
RT_IMPORT(closedir, x_closedir)
RT_IMPORT(basename, x_basename)
RT_DATA(__sF, init_sF)
RT_IMPORT(fopen, x_fopen)
RT_IMPORT(fdopen, x_fdopen)
RT_IMPORT(freopen, x_freopen)
RT_IMPORT(fclose, x_fclose)
RT_IMPORT(fread, x_fread)
RT_IMPORT(fwrite, x_fwrite)
RT_IMPORT(fgets, x_fgets)
RT_IMPORT(fgetc, x_fgetc)
RT_IMPORT(getc, x_fgetc)
RT_IMPORT(ungetc, x_ungetc)
RT_IMPORT(fputc, x_fputc)
RT_IMPORT(putc, x_fputc)
RT_IMPORT(putchar, x_putchar)
RT_IMPORT(fputs, x_fputs)
RT_IMPORT(puts, x_puts)
RT_IMPORT(fseek, x_fseek)
RT_IMPORT(fseeko, x_fseek)
RT_IMPORT(ftell, x_ftell)
RT_IMPORT(ftello, x_ftell)
RT_IMPORT(fflush, x_fflush)
RT_IMPORT(setvbuf, x_setvbuf)
RT_IMPORT(setbuf, x_setbuf)
RT_IMPORT(clearerr, x_clearerr)
RT_IMPORT(flockfile, x_flockfile)
RT_IMPORT(funlockfile, x_funlockfile)
RT_IMPORT(perror, x_perror)
RT_IMPORT(tmpfile, x_tmpfile)
RT_IMPORT(tmpnam, x_tmpnam)
RT_IMPORT(getwc, x_getwc)
RT_IMPORT(putwc, x_putwc)
RT_IMPORT(ungetwc, x_ungetwc)
RT_IMPORT_RAW(printf, i_printf)
RT_IMPORT_RAW(fprintf, i_fprintf)
RT_IMPORT_RAW(vfprintf, i_vfprintf)
RT_IMPORT_RAW(sprintf, i_sprintf)
RT_IMPORT_RAW(vsprintf, i_vsprintf)
RT_IMPORT_RAW(snprintf, i_snprintf)
RT_IMPORT_RAW(vsnprintf, i_vsnprintf)
RT_IMPORT_RAW(sscanf, i_sscanf)
RT_IMPORT_RAW(fscanf, i_fscanf)

// bytes and requests read from host files so far (the read cache's misses and large reads)
void io_read_stats(uint64_t &bytes, uint64_t &requests) { bytes = g_host_read_bytes, requests = g_host_reads; }
// the file system requests so far (times in os::ticks), and the one the game thread waits for
void io_stats(IoStats &s) {
    s.read_bytes = g_host_read_bytes, s.reads = g_host_reads, s.read_ticks = g_host_read_ticks;
    s.opens = g_host_opens, s.open_ticks = g_host_open_ticks, s.stats = g_host_stats, s.stat_ticks = g_host_stat_ticks;
    s.unpacked_bytes = g_unpacked_bytes, s.unpack_ticks = g_unpack_ticks;
}
int io_now() { return g_io_now; }
#ifdef __3DS__
// the boot is over (the title menu runs): the mount index is freed
void io_boot_done() {
    std::lock_guard<std::mutex> lock(g_index_lock);
    if (!g_index_file) return;
    std::fclose(g_index_file);
    g_index_file = nullptr;
    int used = 0;
    for (auto &[name, p] : g_index) {
        used += p.state == IndexPart::kChecked;
        if (p.state != IndexPart::kUnloaded) p.state = IndexPart::kOff;
        p.data = {}, p.ranges = {};
    }
    log("files: the mount index served %llu reads from %d archives", (unsigned long long)g_index_reads, used);
}
#endif

}  // namespace rt
