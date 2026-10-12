// Profile-guided optimisation (CMake -DNOVA3_PGO=generate, see docs/recompiler.md): the
// instrumented build writes its profile when the process ends. exit_process ends it without
// running atexit handlers (TerminateProcess on Windows, svcExitProcess / _Exit on the Switch),
// so it calls pgo_flush() first.
#include "os.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(NOVA3_PGO_GENERATE) && defined(__clang__)

// clang's profile runtime: one file per process (the %p in -fprofile-instr-generate=path)
extern "C" int __llvm_profile_write_file(void);
void rt::os::pgo_flush() { __llvm_profile_write_file(); }

#elif defined(NOVA3_PGO_GENERATE) && defined(__GNUC__)

// GCC: the objects register their counters in the section nova3_gcov (-fprofile-info-section)
// instead of a destructor; each one is written here as a .gcda file of its own, under
// NOVA3_PGO_OUT/run-N/ (one directory per run: gcov-tool merge adds them up). The compiled-in
// name is the build's profile directory + the object path relative to the build directory,
// mangled ('/' -> '#'); only that last part is kept, which is the name -fprofile-use looks for.
extern "C" {
#include <gcov.h>
}
#include <sys/stat.h>

extern "C" const struct gcov_info *const __start_nova3_gcov[] __attribute__((weak));
extern "C" const struct gcov_info *const __stop_nova3_gcov[] __attribute__((weak));

namespace {
struct Out {
    std::string dir;
    FILE *f = nullptr;
};
void on_filename(const char *name, void *arg) {
    Out &o = *static_cast<Out *>(arg);
    if (!name) return;
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    o.f = std::fopen((o.dir + "/" + base).c_str(), "wb");
}
void on_data(const void *data, unsigned n, void *arg) {
    Out &o = *static_cast<Out *>(arg);
    if (o.f) std::fwrite(data, 1, n, o.f);
}
void *on_alloc(unsigned n, void *) { return std::malloc(n); }
}  // namespace

void rt::os::pgo_flush() {
    static bool done;
    if (done || !__start_nova3_gcov) return;
    done = true;
    std::string root = NOVA3_PGO_OUT, dir;
    for (size_t i = root.find('/', root.find(":/") == std::string::npos ? 1 : root.find(":/") + 2);
         i != std::string::npos; i = root.find('/', i + 1))
        mkdir(root.substr(0, i).c_str(), 0777);
    mkdir(root.c_str(), 0777);
    for (int n = 0; n < 10000; n++) {
        dir = root + "/run-" + std::to_string(n);
        if (mkdir(dir.c_str(), 0777) == 0) break;
    }
    int files = 0;
    for (const gcov_info *const *p = __start_nova3_gcov; p < __stop_nova3_gcov; p++) {
        Out o{dir};
        __gcov_info_to_gcda(*p, on_filename, on_data, on_alloc, &o);
        if (o.f) {
            std::fclose(o.f);
            files++;
        }
    }
    std::fprintf(stderr, "pgo: %d profile files written to %s\n", files, dir.c_str());
}

#else

void rt::os::pgo_flush() {}

#endif
