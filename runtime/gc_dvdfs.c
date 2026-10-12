/* The GameCube disc's file system as newlib device "dvd:" (read-only): the disc image's file
 * table (FST, tools/gc_disc.py writes it) is read once at mount, paths are looked up in it, and
 * reads go through libogc2's DVD driver into a 64 KB bounce block (DVD DMA needs 32-byte
 * alignment), which also serves later reads from the same block.
 *
 * FST (the disc header's offset 0x424 and size 0x428): 12-byte entries, then their names. An
 * entry: flags << 24 | name offset; a file's disc offset or a directory's parent; a file's size
 * or the index past a directory's last descendant. Entry 0 is the root, its third word the
 * entry count. */
#include "gc_dvdfs.h"

#include <gccore.h>
#include <ogc/dvd.h>
#include <ogc/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

typedef struct Entry {
    uint32_t name, a, b;
} Entry;

#define BLOCK (64u * 1024u)

static Entry *g_fst;
static const char *g_names;
static uint32_t g_count;
static uint8_t *g_block;          /* 32-byte aligned bounce block */
static uint32_t g_block_at = ~0u; /* disc offset of g_block's contents */
static mutex_t g_lock = LWP_MUTEX_NULL;

static int disc_read(void *dst, uint32_t len, uint32_t offset) {
    /* DVD_ReadAbsPrio: 32-byte aligned buffer, length a multiple of 32, offset of 4. A command block
     * per call: the pager reads code from inside a fault, maybe while this thread was in a read
     * (libogc's queue orders the commands). */
    dvdcmdblk cmd;
    return DVD_ReadAbsPrio(&cmd, dst, (len + 31u) & ~31u, offset, 2) >= 0;
}

static int is_dir(const Entry *e) { return e->name >> 24 != 0; }

/* the entry for a path below the root ("" or "/" is the root), or -1 */
static int lookup(const char *path) {
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    uint32_t dir = 0;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char *end = strchr(path, '/');
        const size_t n = end ? (size_t)(end - path) : strlen(path);
        uint32_t i = dir + 1, found = 0;
        while (i < g_fst[dir].b) {
            const char *name = g_names + (g_fst[i].name & 0xffffffu);
            if (strlen(name) == n && strncasecmp(name, path, n) == 0) {
                found = i;
                break;
            }
            i = is_dir(&g_fst[i]) ? g_fst[i].b : i + 1;
        }
        if (!found) return -1;
        dir = found;
        path += n;
        if (*path && !is_dir(&g_fst[dir])) return -1;
    }
    return (int)dir;
}

typedef struct File {
    uint32_t start, size, pos;
} File;

static void fill_stat(const Entry *e, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_mode = is_dir(e) ? S_IFDIR | 0555 : S_IFREG | 0444;
    st->st_size = is_dir(e) ? 0 : e->b;
    st->st_nlink = 1;
    st->st_blksize = 2048;
}

static int dvd_open(struct _reent *r, void *fs, const char *path, int flags, int mode) {
    (void)mode;
    if ((flags & O_ACCMODE) != O_RDONLY) {
        r->_errno = EROFS;
        return -1;
    }
    const int i = lookup(path);
    if (i < 0 || is_dir(&g_fst[i])) {
        r->_errno = i < 0 ? ENOENT : EISDIR;
        return -1;
    }
    File *f = (File *)fs;
    f->start = g_fst[i].a;
    f->size = g_fst[i].b;
    f->pos = 0;
    return (int)(intptr_t)fs;
}

static int dvd_close(struct _reent *r, void *fd) {
    (void)r, (void)fd;
    return 0;
}

static ssize_t dvd_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    File *f = (File *)fd;
    if (f->pos >= f->size) return 0;
    if (len > f->size - f->pos) len = f->size - f->pos;
    size_t done = 0;
    LWP_MutexLock(g_lock);
    while (done < len) {
        const uint32_t at = f->start + f->pos + (uint32_t)done;
        const uint32_t block = at & ~(BLOCK - 1u);
        if (block != g_block_at) {
            g_block_at = ~0u;
            if (!disc_read(g_block, BLOCK, block)) {
                LWP_MutexUnlock(g_lock);
                r->_errno = EIO;
                return done ? (ssize_t)done : -1;
            }
            g_block_at = block;
        }
        uint32_t n = BLOCK - (at - block);
        if (n > len - done) n = (uint32_t)(len - done);
        memcpy(ptr + done, g_block + (at - block), n);
        done += n;
    }
    LWP_MutexUnlock(g_lock);
    f->pos += (uint32_t)done;
    return (ssize_t)done;
}

static off_t dvd_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    File *f = (File *)fd;
    const off_t base = dir == SEEK_SET ? 0 : dir == SEEK_CUR ? (off_t)f->pos : (off_t)f->size;
    if (base + pos < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    f->pos = (uint32_t)(base + pos);
    return f->pos;
}

static int dvd_fstat(struct _reent *r, void *fd, struct stat *st) {
    (void)r;
    const File *f = (const File *)fd;
    const Entry e = {0, f->start, f->size};
    fill_stat(&e, st);
    return 0;
}

static int dvd_stat(struct _reent *r, const char *path, struct stat *st) {
    const int i = lookup(path);
    if (i < 0) {
        r->_errno = ENOENT;
        return -1;
    }
    fill_stat(&g_fst[i], st);
    return 0;
}

typedef struct Dir {
    uint32_t dir, next;
} Dir;

static DIR_ITER *dvd_diropen(struct _reent *r, DIR_ITER *state, const char *path) {
    const int i = lookup(path);
    if (i < 0 || !is_dir(&g_fst[i])) {
        r->_errno = i < 0 ? ENOENT : ENOTDIR;
        return NULL;
    }
    Dir *d = (Dir *)state->dirStruct;
    d->dir = (uint32_t)i;
    d->next = (uint32_t)i + 1;
    return state;
}

static int dvd_dirreset(struct _reent *r, DIR_ITER *state) {
    (void)r;
    Dir *d = (Dir *)state->dirStruct;
    d->next = d->dir + 1;
    return 0;
}

static int dvd_dirnext(struct _reent *r, DIR_ITER *state, char *name, struct stat *st) {
    Dir *d = (Dir *)state->dirStruct;
    if (d->next >= g_fst[d->dir].b) {
        r->_errno = ENOENT;
        return -1;
    }
    const Entry *e = &g_fst[d->next];
    strcpy(name, g_names + (e->name & 0xffffffu));
    if (st) fill_stat(e, st);
    d->next = is_dir(e) ? e->b : d->next + 1;
    return 0;
}

static int dvd_dirclose(struct _reent *r, DIR_ITER *state) {
    (void)r, (void)state;
    return 0;
}

static const devoptab_t g_devoptab = {
    .name = "dvd",
    .structSize = sizeof(File),
    .open_r = dvd_open,
    .close_r = dvd_close,
    .read_r = dvd_read,
    .seek_r = dvd_seek,
    .fstat_r = dvd_fstat,
    .stat_r = dvd_stat,
    .dirStateSize = sizeof(Dir),
    .diropen_r = dvd_diropen,
    .dirreset_r = dvd_dirreset,
    .dirnext_r = dvd_dirnext,
    .dirclose_r = dvd_dirclose,
};

int gc_dvdfs_mount(void) {
    DVD_Init();
    if (DVD_Mount() < 0) return 0;
    g_block = (uint8_t *)memalign(32, BLOCK);
    LWP_MutexInit(&g_lock, false);
    if (!disc_read(g_block, 0x440, 0)) return 0;
    uint32_t fst_offset, fst_size;
    memcpy(&fst_offset, g_block + 0x424, 4);
    memcpy(&fst_size, g_block + 0x428, 4);
    g_fst = (Entry *)memalign(32, (fst_size + 31u) & ~31u);
    if (!g_fst || !disc_read(g_fst, fst_size, fst_offset)) return 0;
    g_count = g_fst[0].b;
    g_names = (const char *)(g_fst + g_count);
    AddDevice(&g_devoptab);
    return 1;
}

uint32_t gc_dvdfs_disc_offset(const char *path, uint32_t *size) {
    const int i = lookup(path);
    if (i < 0 || is_dir(&g_fst[i])) return 0;
    if (size) *size = g_fst[i].b;
    return g_fst[i].a;
}

int gc_dvdfs_read_raw(void *dst, uint32_t len, uint32_t offset) { return disc_read(dst, len, offset); }
