// Native half of tests/difftest.py: run each recompiled snippet from the given initial
// state and write the final state (+ changed bytes of the memory window).
#include "runtime.h"

#include <cstdio>
#include <vector>

#pragma pack(push, 1)
struct Case {
    uint32_t snippet, r[16], n, z, c, v, fpscr;
    uint8_t vfp[256];
};
struct Result {
    uint32_t r[16], n, z, c, v, fpscr;
    uint8_t vfp[256];
    uint32_t changed;
};
#pragma pack(pop)

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = std::fopen(argv[1], "rb"), *out = std::fopen(argv[2], "wb");
    if (!in || !out) return 3;
    uint32_t win, win_size, count;
    std::fread(&win, 4, 1, in);
    std::fread(&win_size, 4, 1, in);
    std::vector<uint8_t> window(win_size);
    std::fread(window.data(), 1, win_size, in);
    std::fread(&count, 4, 1, in);

    rt::init_memory();
    rt::commit(win, win_size);
    Cpu cpu{};
    for (uint32_t k = 0; k < count; k++) {
        Case cs;
        if (std::fread(&cs, sizeof cs, 1, in) != 1) return 4;
        std::memcpy(HOST(win), window.data(), win_size);
        std::memcpy(cpu.r, cs.r, sizeof cpu.r);
        cpu.n = cs.n, cpu.z = cs.z, cpu.c = cs.c, cpu.v = cs.v, cpu.fpscr = cs.fpscr;
        cpu.ex_addr = ~0u;
        std::memcpy(&cpu.vr, cs.vfp, 256);
        rt_dispatch(&cpu, g_load_base + cs.snippet);

        Result r{};
        std::memcpy(r.r, cpu.r, sizeof r.r);
        r.n = cpu.n, r.z = cpu.z, r.c = cpu.c, r.v = cpu.v, r.fpscr = cpu.fpscr;
        std::memcpy(r.vfp, &cpu.vr, 256);
        std::vector<std::pair<uint16_t, uint8_t>> changed;
        for (uint32_t i = 0; i < win_size; i++)
            if (HOST(win)[i] != window[i]) changed.push_back({uint16_t(i), HOST(win)[i]});
        r.changed = uint32_t(changed.size());
        std::fwrite(&r, sizeof r, 1, out);
        for (auto &[off, val] : changed) {
            std::fwrite(&off, 2, 1, out);
            std::fwrite(&val, 1, 1, out);
        }
    }
    std::fclose(out);
    return 0;
}
