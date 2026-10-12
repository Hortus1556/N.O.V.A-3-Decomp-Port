/* The GameCube disc's files as newlib device "dvd:" (gc_dvdfs.c). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the disc in the drive (Dolphin: the inserted image) and adds "dvd:"; 0 if none. */
int gc_dvdfs_mount(void);
/* A file's offset on the disc and its size (for direct reads), 0 if there is no such file. */
uint32_t gc_dvdfs_disc_offset(const char *path, uint32_t *size);
/* Reads the disc directly: dst 32-byte aligned, len rounded up to 32, offset a multiple of 4. */
int gc_dvdfs_read_raw(void *dst, uint32_t len, uint32_t offset);

#ifdef __cplusplus
}
#endif
