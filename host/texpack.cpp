// HD textures. The game decodes its ETC1 textures on the CPU and uploads them with
// glTexImage2D as RGB, followed by its own smaller mip levels (or glGenerateMipmap). The pack
// (tools/texpack.py) holds, for each texture worth upscaling, the CRC-32 and size of that
// level-0 upload and a replacement twice as wide and high, BC7-compressed with all its mip
// levels:
//
//   header  "N3HD", version 1, entry count
//   entries crc32, width, height, GL format, width, height, mip count, offset (u64), size
//   data    the replacements' mip levels, largest first
//
// When a level-0 upload matches an entry, the replacement's levels are uploaded instead, and
// the game's later level uploads and glGenerateMipmap for that texture object are dropped.
// Everything else goes through untouched, so turning the pack off (or a texture missing from
// it) gives the original.
#include "texpack.h"

#include "runtime.h"

#include "sdl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace texpack {
namespace {

constexpr uint32_t kTexture2D = 0x0DE1, kTextureBinding2D = 0x8069, kRgb = 0x1907, kUnsignedByte = 0x1401;
constexpr uint32_t kBc7 = 0x8E8C;  // GL_COMPRESSED_RGBA_BPTC_UNORM_EXT

struct Entry {
    uint32_t format, width, height, mips;
    uint64_t offset;
    uint32_t size;
};

std::FILE *g_file = nullptr;
std::unordered_map<uint64_t, Entry> g_entries;   // key(crc, width, height)
std::unordered_set<uint32_t> g_sizes;            // width << 16 | height of the originals
std::unordered_set<uint32_t> g_replaced;         // texture objects holding a replacement
std::vector<uint8_t> g_buffer;
int g_count = 0;

using GetIntegerv = void (*)(uint32_t, int *);
using CompressedTexImage2D = void (*)(uint32_t, int, uint32_t, int, int, int, int, const void *);
GetIntegerv p_get_integerv;
CompressedTexImage2D p_compressed_tex_image;

uint64_t key(uint32_t crc, uint32_t w, uint32_t h) { return uint64_t(crc) << 32 | (w & 0xffff) << 16 | (h & 0xffff); }

// CRC-32 (zlib's: reflected, 0xEDB88320), slicing by 8.
uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[8][256];
    static bool ready = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int s = 1; s < 8; s++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
        return true;
    }();
    (void)ready;
    uint32_t c = ~0u;
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= c;
        c = t[7][a & 0xff] ^ t[6][(a >> 8) & 0xff] ^ t[5][(a >> 16) & 0xff] ^ t[4][a >> 24] ^ t[3][b & 0xff] ^
            t[2][(b >> 8) & 0xff] ^ t[1][(b >> 16) & 0xff] ^ t[0][b >> 24];
    }
    while (n--) c = t[0][(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

uint32_t bound_texture() {
    int id = 0;
    p_get_integerv(kTextureBinding2D, &id);
    return uint32_t(id);
}

bool upload(const Entry &e) {
    g_buffer.resize(e.size);
#ifdef _WIN32
    const bool seeked = _fseeki64(g_file, int64_t(e.offset), SEEK_SET) == 0;
#else
    const bool seeked = fseeko(g_file, off_t(e.offset), SEEK_SET) == 0;
#endif
    if (!seeked || std::fread(g_buffer.data(), 1, e.size, g_file) != e.size)
        return false;
    size_t off = 0;
    uint32_t w = e.width, h = e.height;
    for (uint32_t level = 0; level < e.mips; level++) {
        const size_t size = size_t((w + 3) / 4) * ((h + 3) / 4) * 16;  // BC7: 16 bytes per 4x4 block
        if (off + size > g_buffer.size()) return false;
        p_compressed_tex_image(kTexture2D, int(level), e.format, int(w), int(h), 0, int(size), g_buffer.data() + off);
        off += size;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return true;
}

bool tex_image(uint32_t target, int level, int internal_format, int w, int h, uint32_t format, uint32_t type,
               const void *pixels) {
    if (target != kTexture2D) return false;
    const uint32_t id = bound_texture();
    if (level > 0) return g_replaced.count(id) != 0;  // the original's own mip levels
    g_replaced.erase(id);  // a new level 0: whatever the object held before is gone
    if (!pixels || type != kUnsignedByte || format != kRgb || internal_format != int(kRgb) || w % 4 ||
        !g_sizes.count(uint32_t(w) << 16 | uint32_t(h)))
        return false;
    const uint32_t crc = crc32(static_cast<const uint8_t *>(pixels), size_t(w) * h * 3);
    auto it = g_entries.find(key(crc, uint32_t(w), uint32_t(h)));
    if (it == g_entries.end() || !upload(it->second)) return false;
    g_replaced.insert(id);
    static const bool trace = std::getenv("NOVA3_TRACE_TEXTURES") != nullptr;
    if (trace) rt::log("textures: %08x_%dx%d -> %ux%u", crc, w, h, it->second.width, it->second.height);
    if (++g_count % 250 == 0) rt::log("textures: %d replaced so far", g_count);
    return true;
}

bool generate_mipmap(uint32_t target) { return target == kTexture2D && g_replaced.count(bound_texture()) != 0; }

}  // namespace

bool init(const std::string &path) {
    using GetString = const char *(*)(uint32_t);
    const auto get_string = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"));
    const char *ext = get_string ? get_string(0x1F03 /*GL_EXTENSIONS*/) : nullptr;
    if (!ext || !std::strstr(ext, "GL_EXT_texture_compression_bptc")) {
        rt::log("textures: no BC7 support (GL_EXT_texture_compression_bptc): HD textures off");
        return false;
    }
    p_get_integerv = reinterpret_cast<GetIntegerv>(SDL_GL_GetProcAddress("glGetIntegerv"));
    p_compressed_tex_image = reinterpret_cast<CompressedTexImage2D>(SDL_GL_GetProcAddress("glCompressedTexImage2D"));
    g_file = std::fopen(path.c_str(), "rb");
    if (!g_file || !p_get_integerv || !p_compressed_tex_image) {
        rt::log("textures: %s not found: HD textures off", path.c_str());
        return false;
    }
    char magic[4];
    uint32_t version = 0, count = 0;
    if (std::fread(magic, 1, 4, g_file) != 4 || std::memcmp(magic, "N3HD", 4) != 0 || std::fread(&version, 4, 1, g_file) != 1 ||
        version != 1 || std::fread(&count, 4, 1, g_file) != 1) {
        rt::log("textures: %s is not an HD texture pack", path.c_str());
        std::fclose(g_file);
        g_file = nullptr;
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t f[7];
        uint64_t offset;
        uint32_t size;
        if (std::fread(f, 4, 7, g_file) != 7 || std::fread(&offset, 8, 1, g_file) != 1 || std::fread(&size, 4, 1, g_file) != 1)
            break;
        if (f[3] != kBc7) continue;
        g_entries[key(f[0], f[1], f[2])] = Entry{f[3], f[4], f[5], f[6], offset, size};
        g_sizes.insert(f[1] << 16 | f[2]);
    }
    rt::gl_set_texture_hooks({tex_image, generate_mipmap});
    rt::log("textures: HD pack %s, %zu textures", path.c_str(), g_entries.size());
    return true;
}

}  // namespace texpack
