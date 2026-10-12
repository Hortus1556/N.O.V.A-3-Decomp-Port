// A writable file system in memory, newlib device "ram:" (GameCube): the player's files when no
// SD card is there (Dolphin has no GameCube SD adapter): saves, settings, the game's scratch
// files. Lost when the console is switched off. Files and directories live in host memory; the
// game writes few and small ones (saves, analytics files).
#include "gc_ramfs.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <vector>

namespace {

std::mutex g_lock;
std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> g_files;
std::set<std::string> g_dirs = {""};

// "ram:/a//b/" -> "a/b"
std::string norm(const char *path) {
    if (const char *c = std::strchr(path, ':')) path = c + 1;
    std::string out;
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        if (e > p) {
            const std::string part(p, size_t(e - p));
            if (part == "..") { const size_t s = out.rfind('/'); out.erase(s == std::string::npos ? 0 : s); }
            else if (part != ".") out += (out.empty() ? "" : "/") + part;
        }
        p = e;
    }
    return out;
}

std::string parent(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}

struct File {
    std::shared_ptr<std::vector<uint8_t>> *data;  // (placement: newlib gives raw storage)
    size_t pos;
    bool append;
};

int ram_open(struct _reent *r, void *fs, const char *path, int flags, int) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string p = norm(path);
    auto it = g_files.find(p);
    if (it == g_files.end()) {
        if (!(flags & O_CREAT) || g_dirs.count(p) || !g_dirs.count(parent(p))) {
            r->_errno = g_dirs.count(p) ? EISDIR : ENOENT;
            return -1;
        }
        it = g_files.emplace(p, std::make_shared<std::vector<uint8_t>>()).first;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        r->_errno = EEXIST;
        return -1;
    }
    if (flags & O_TRUNC) it->second->clear();
    auto *f = static_cast<File *>(fs);
    f->data = new std::shared_ptr<std::vector<uint8_t>>(it->second);
    f->pos = 0;
    f->append = (flags & O_APPEND) != 0;
    return int(intptr_t(fs));
}

int ram_close(struct _reent *, void *fd) {
    auto *f = static_cast<File *>(fd);
    std::lock_guard<std::mutex> lock(g_lock);
    delete f->data;
    return 0;
}

ssize_t ram_write(struct _reent *, void *fd, const char *ptr, size_t len) {
    auto *f = static_cast<File *>(fd);
    std::lock_guard<std::mutex> lock(g_lock);
    auto &d = **f->data;
    if (f->append) f->pos = d.size();
    if (f->pos + len > d.size()) d.resize(f->pos + len);
    std::memcpy(d.data() + f->pos, ptr, len);
    f->pos += len;
    return ssize_t(len);
}

ssize_t ram_read(struct _reent *, void *fd, char *ptr, size_t len) {
    auto *f = static_cast<File *>(fd);
    std::lock_guard<std::mutex> lock(g_lock);
    const auto &d = **f->data;
    if (f->pos >= d.size()) return 0;
    len = std::min(len, d.size() - f->pos);
    std::memcpy(ptr, d.data() + f->pos, len);
    f->pos += len;
    return ssize_t(len);
}

off_t ram_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto *f = static_cast<File *>(fd);
    std::lock_guard<std::mutex> lock(g_lock);
    const off_t base = dir == SEEK_SET ? 0 : dir == SEEK_CUR ? off_t(f->pos) : off_t((*f->data)->size());
    if (base + pos < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    f->pos = size_t(base + pos);
    return off_t(f->pos);
}

void fill(struct stat *st, bool dir, size_t size) {
    std::memset(st, 0, sizeof *st);
    st->st_mode = dir ? S_IFDIR | 0777 : S_IFREG | 0666;
    st->st_size = off_t(size);
    st->st_nlink = 1;
}

int ram_fstat(struct _reent *, void *fd, struct stat *st) {
    std::lock_guard<std::mutex> lock(g_lock);
    fill(st, false, (*static_cast<File *>(fd)->data)->size());
    return 0;
}

int ram_stat(struct _reent *r, const char *path, struct stat *st) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string p = norm(path);
    if (g_dirs.count(p)) return fill(st, true, 0), 0;
    auto it = g_files.find(p);
    if (it == g_files.end()) {
        r->_errno = ENOENT;
        return -1;
    }
    fill(st, false, it->second->size());
    return 0;
}

int ram_unlink(struct _reent *r, const char *path) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_files.erase(norm(path))) {
        r->_errno = ENOENT;
        return -1;
    }
    return 0;
}

int ram_rename(struct _reent *r, const char *from, const char *to) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string a = norm(from), b = norm(to);
    auto it = g_files.find(a);
    if (it == g_files.end()) {
        r->_errno = ENOENT;
        return -1;
    }
    auto data = it->second;
    g_files.erase(it);
    g_files[b] = data;
    return 0;
}

int ram_mkdir(struct _reent *r, const char *path, int) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string p = norm(path);
    if (g_dirs.count(p) || g_files.count(p)) {
        r->_errno = EEXIST;
        return -1;
    }
    if (!g_dirs.count(parent(p))) {
        r->_errno = ENOENT;
        return -1;
    }
    g_dirs.insert(p);
    return 0;
}

int ram_rmdir(struct _reent *r, const char *path) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string p = norm(path);
    const std::string prefix = p + "/";
    for (const auto &f : g_files)
        if (f.first.compare(0, prefix.size(), prefix) == 0) {
            r->_errno = ENOTEMPTY;
            return -1;
        }
    if (p.empty() || !g_dirs.erase(p)) {
        r->_errno = ENOENT;
        return -1;
    }
    return 0;
}

struct Dir {
    std::vector<std::pair<std::string, bool>> *entries;  // name, is a directory
    size_t next;
};

DIR_ITER *ram_diropen(struct _reent *r, DIR_ITER *state, const char *path) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string p = norm(path);
    if (!g_dirs.count(p)) {
        r->_errno = ENOENT;
        return nullptr;
    }
    auto *list = new std::vector<std::pair<std::string, bool>>;
    const std::string prefix = p.empty() ? "" : p + "/";
    auto child = [&](const std::string &full) -> std::string {
        if (full.size() <= prefix.size() || full.compare(0, prefix.size(), prefix) != 0) return "";
        const std::string rest = full.substr(prefix.size());
        return rest.find('/') == std::string::npos ? rest : "";
    };
    for (const auto &d : g_dirs) if (const std::string n = child(d); !n.empty()) list->push_back({n, true});
    for (const auto &f : g_files) if (const std::string n = child(f.first); !n.empty()) list->push_back({n, false});
    auto *d = static_cast<Dir *>(state->dirStruct);
    d->entries = list;
    d->next = 0;
    return state;
}

int ram_dirreset(struct _reent *, DIR_ITER *state) {
    static_cast<Dir *>(state->dirStruct)->next = 0;
    return 0;
}

int ram_dirnext(struct _reent *r, DIR_ITER *state, char *name, struct stat *st) {
    auto *d = static_cast<Dir *>(state->dirStruct);
    if (d->next >= d->entries->size()) {
        r->_errno = ENOENT;
        return -1;
    }
    const auto &e = (*d->entries)[d->next++];
    std::strcpy(name, e.first.c_str());
    if (st) fill(st, e.second, 0);
    return 0;
}

int ram_dirclose(struct _reent *, DIR_ITER *state) {
    delete static_cast<Dir *>(state->dirStruct)->entries;
    return 0;
}

int ram_ftruncate(struct _reent *, void *fd, off_t len) {
    std::lock_guard<std::mutex> lock(g_lock);
    (*static_cast<File *>(fd)->data)->resize(size_t(len));
    return 0;
}

int ram_fsync(struct _reent *, void *) { return 0; }

devoptab_t make_devoptab() {
    devoptab_t d{};
    d.name = "ram";
    d.structSize = sizeof(File);
    d.open_r = ram_open;
    d.close_r = ram_close;
    d.write_r = ram_write;
    d.read_r = ram_read;
    d.seek_r = ram_seek;
    d.fstat_r = ram_fstat;
    d.stat_r = ram_stat;
    d.unlink_r = ram_unlink;
    d.rename_r = ram_rename;
    d.mkdir_r = ram_mkdir;
    d.dirStateSize = sizeof(Dir);
    d.diropen_r = ram_diropen;
    d.dirreset_r = ram_dirreset;
    d.dirnext_r = ram_dirnext;
    d.dirclose_r = ram_dirclose;
    d.ftruncate_r = ram_ftruncate;
    d.fsync_r = ram_fsync;
    d.rmdir_r = ram_rmdir;
    d.lstat_r = ram_stat;
    return d;
}

}  // namespace

void gc_ramfs_mount() {
    static const devoptab_t dev = make_devoptab();
    AddDevice(&dev);
}
