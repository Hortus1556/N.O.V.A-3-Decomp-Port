// The DSP firmware that libctru's ndsp loads into the DSP (sdmc:/3ds/dspfirm.cdc). Games carry
// their own copy; homebrew has none and takes the console's: DSP1 (github.com/zoogie/DSP1)
// opens the HOME Menu's ExeFS .code through FS (the title-content archive 0x2345678A, the HOME
// Menu's title id per region, on the NAND), unpacks it (the ExeFS's backward LZSS), finds the
// firmware by its "DSP1" magic (at 0x100 of the file, after its RSA signature) and writes the
// size noted there from the signature on. Done here the same way at the first start when the
// file is missing, and checked by the SHA-256 of each of its segments, which DSP1 only warns
// about. The archive needs the exheader's FS access (host/3ds/nova3.rsf: one of the bits FS
// checks for 0x2345678A). Without the file the sound goes through CSND (host/audio.cpp).
#include "os.h"
#include "runtime.h"

#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

// SHA-256 (FIPS 180-4), for the firmware's segment hashes
void sha256(const uint8_t *data, size_t n, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto ror = [](uint32_t x, int s) { return x >> s | x << (32 - s); };
    std::vector<uint8_t> m(data, data + n);  // the message, padded to 64-byte blocks
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(uint8_t(uint64_t(n) * 8 >> (8 * i)));
    for (size_t at = 0; at < m.size(); at += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(m[at + 4 * i]) << 24 | m[at + 4 * i + 1] << 16 | m[at + 4 * i + 2] << 8 | m[at + 4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
                   (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    for (int i = 0; i < 32; i++) out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
}

// The ExeFS's .code compression (ctrtool's lzss, as DSP1 uses it): a footer of (top and bottom,
// the extra size); unpacked from the end backwards, in place over a copy. False if it runs out of
// bounds (then the code was not compressed, or is damaged).
bool blz_unpack(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    const size_t n = in.size();
    if (n < 8) return false;
    const uint32_t top_bottom = le32(&in[n - 8]), extra = le32(&in[n - 4]);
    const size_t header = top_bottom >> 24, end = top_bottom & 0xffffff;
    if (header < 8 || header > n || end > n || extra > (64u << 20)) return false;
    out.assign(n + extra, 0);
    std::memcpy(out.data(), in.data(), n);
    size_t index = n - header, stop = n - end, at = out.size();
    while (index > stop) {
        const uint8_t control = in[--index];
        for (int i = 0; i < 8 && index > stop && at > 0; i++) {
            if (control & (0x80 >> i)) {  // a copy of 3-18 bytes from 3-4098 bytes further on
                if (index < 2) return false;
                index -= 2;
                const uint32_t pair = in[index] | in[index + 1] << 8;
                const size_t len = ((pair >> 12) & 15) + 3, back = (pair & 0x0fff) + 2;
                if (at < len) return false;
                for (size_t j = 0; j < len; j++) {
                    if (at + back >= out.size()) return false;
                    const uint8_t v = out[at + back];
                    out[--at] = v;
                }
            } else {
                if (at < 1) return false;
                out[--at] = in[--index];
            }
        }
    }
    return true;
}

// The firmware in an unpacked .code: the first "DSP1" whose noted size fits and whose segments'
// SHA-256s match (segment headers at 0x120 of the file, 0x30 bytes each: offset, address, size,
// the memory type, then the hash; up to 10, the first of size 0 ends them)
bool find_firmware(const std::vector<uint8_t> &code, std::vector<uint8_t> &fw, std::string &why) {
    why = "no DSP1 magic";
    for (size_t at = 0x100; at + 0x300 <= code.size(); at++) {
        if (std::memcmp(&code[at], "DSP1", 4)) continue;
        const uint8_t *base = &code[at - 0x100];
        const uint32_t size = le32(&code[at + 4]);
        if (size < 0x300 || size > (1u << 20) || at - 0x100 + size > code.size()) {
            why = "a DSP1 magic with a size that does not fit";
            continue;
        }
        bool ok = true;
        int segments = 0;
        for (int i = 0; i < 10 && ok; i++) {
            const uint8_t *s = base + 0x120 + 0x30 * i;
            const uint32_t offset = le32(s), length = le32(s + 8);
            if (!length) break;
            uint8_t hash[32];
            ok = offset < size && length <= size - offset;
            if (ok) sha256(base + offset, length, hash);
            ok = ok && !std::memcmp(hash, s + 0x10, 32);
            segments++;
        }
        if (!ok || !segments) {
            why = "a DSP1 magic whose segment hashes do not match";
            continue;
        }
        fw.assign(base, base + size);
        return true;
    }
    return false;
}

// a title's ExeFS .code through the title-content archive (DSP1's openCode)
Result read_code(u64 title, FS_MediaType media, std::vector<uint8_t> &code) {
    const u32 archive[4] = {u32(title), u32(title >> 32), u32(media), 0};
    static const u32 file[5] = {0, 0, 2 /* ExeFS */, 0x646f632e /* ".cod" */, 0x65 /* "e" */};
    Handle h = 0;
    Result rc = FSUSER_OpenFileDirectly(&h, FS_ArchiveID(0x2345678a), FS_Path{PATH_BINARY, sizeof archive, archive},
                                        FS_Path{PATH_BINARY, sizeof file, file}, FS_OPEN_READ, 0);
    if (R_FAILED(rc)) return rc;
    u64 size = 0;
    rc = FSFILE_GetSize(h, &size);
    if (R_SUCCEEDED(rc) && (size < 8 || size > (32u << 20))) rc = -1;
    u32 got = 0;
    if (R_SUCCEEDED(rc)) {
        code.resize(size_t(size));
        rc = FSFILE_Read(h, &got, 0, code.data(), u32(size));
        if (R_SUCCEEDED(rc) && got != size) rc = -1;
    }
    FSFILE_Close(h);
    return rc;
}

}  // namespace

// Makes sure sdmc:/3ds/dspfirm.cdc is there before ndspInit: extracted from the HOME Menu when it
// is missing. NOVA3_DSPFIRM_CODE (a test script's env) names a .code file to take it from
// instead (emulators have no HOME Menu). Logs the outcome.
void dspfirm_ensure() {
    static const char kPath[] = "sdmc:/3ds/dspfirm.cdc";
    struct stat st;
    if (stat(kPath, &st) == 0 && st.st_size > 0) {
        rt::log("audio: DSP firmware %s (%ld bytes)", kPath, long(st.st_size));
        return;
    }
    static const struct { u64 title; const char *name; } kHomeMenus[] = {
        {0x0004003000008f02ull, "USA"}, {0x0004003000008202ull, "JPN"}, {0x0004003000009802ull, "EUR"},
        {0x000400300000a102ull, "CHN"}, {0x000400300000a902ull, "KOR"}, {0x000400300000b102ull, "TWN"}};
    const int64_t t0 = rt::os::monotonic_ns();
    std::vector<uint8_t> code, unpacked, fw;
    std::string tried;
    const char *test = std::getenv("NOVA3_DSPFIRM_CODE");
    for (size_t i = 0; i < (test ? 1 : std::size(kHomeMenus)) && fw.empty(); i++) {
        std::string why;
        if (test) {
            FILE *f = std::fopen(test, "rb");
            code.clear();
            if (f) {
                uint8_t b[65536];
                for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) code.insert(code.end(), b, b + n);
                std::fclose(f);
            }
            why = code.empty() ? "cannot read it" : "";
        } else if (const Result rc = read_code(kHomeMenus[i].title, MEDIATYPE_NAND, code); R_FAILED(rc)) {
            char b[64];
            std::snprintf(b, sizeof b, "not readable (%08lx)", (unsigned long)rc);
            why = b;
        }
        bool unpacked_ok = false;  // (found in the unpacked code, else in the code as stored)
        if (why.empty()) {
            unpacked_ok = blz_unpack(code, unpacked) && find_firmware(unpacked, fw, why);
            if (!unpacked_ok) find_firmware(code, fw, why);
        }
        if (fw.empty()) {
            tried += std::string(tried.empty() ? "" : "; ") + (test ? test : kHomeMenus[i].name) + ": " + why;
            continue;
        }
        FILE *f = std::fopen(kPath, "wb");
        const bool written = f && std::fwrite(fw.data(), 1, fw.size(), f) == fw.size();
        if (f && std::fclose(f) != 0) f = nullptr;
        if (!written || !f) {
            std::remove(kPath);
            rt::log("audio: DSP firmware found in %s's code but %s could not be written", test ? test : kHomeMenus[i].name, kPath);
            return;
        }
        rt::log("audio: DSP firmware extracted from %s (%s code, %zu bytes, segment hashes checked) to %s in %.2f s",
                test ? test : (std::string("the HOME Menu (") + kHomeMenus[i].name + ")").c_str(),
                unpacked_ok ? "unpacked" : "stored", fw.size(), kPath, (rt::os::monotonic_ns() - t0) / 1e9);
        return;
    }
    rt::log("audio: no %s and none extracted (%s)", kPath, tried.c_str());
}
