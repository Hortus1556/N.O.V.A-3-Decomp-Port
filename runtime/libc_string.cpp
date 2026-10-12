// <string.h>, <ctype.h>, <wchar.h>, <wctype.h>, <stdlib.h> conversions, qsort, rand48.
// All pointers are guest addresses. Android's wchar_t is 32-bit; the C locale is ASCII.
#include "runtime.h"
#include "tls.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include <vector>

namespace rt {
namespace {

using u32 = uint32_t;

// ---- memory / strings ------------------------------------------------------------
u32 g_memcpy(u32 d, u32 s, u32 n) { std::memcpy(ptr(d), ptr(s), n); return d; }
u32 g_memmove(u32 d, u32 s, u32 n) { std::memmove(ptr(d), ptr(s), n); return d; }
u32 g_memset(u32 d, int c, u32 n) { std::memset(ptr(d), c, n); return d; }
int g_memcmp(u32 a, u32 b, u32 n) { return std::memcmp(ptr(a), ptr(b), n); }
u32 g_memchr(u32 s, int c, u32 n) {
    const void *p = std::memchr(ptr(s), c, n);
    return p ? gaddr(p) : 0;
}
u32 g_memmem(u32 h, u32 hn, u32 n, u32 nn) {
    if (!nn) return h;
    const char *hp = ptr(h), *np = ptr(n);
    for (u32 i = 0; i + nn <= hn; i++)
        if (!std::memcmp(hp + i, np, nn)) return h + i;
    return 0;
}
u32 g_strlen(u32 s) { return u32(std::strlen(str(s))); }
int g_strcmp(u32 a, u32 b) { return std::strcmp(str(a), str(b)); }
int g_strncmp(u32 a, u32 b, u32 n) { return std::strncmp(str(a), str(b), n); }
// ASCII case folding (bionic's C locale), independent of the host locale
int casecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = uint8_t(a[i]), cb = uint8_t(b[i]);
        ca += (ca - 'A' < 26u) << 5;  // 'A'-'Z' -> 'a'-'z' (unsigned compare: one branch-free test)
        cb += (cb - 'A' < 26u) << 5;
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}
int g_strcasecmp(u32 a, u32 b) { return casecmp(str(a), str(b), size_t(-1)); }
int g_strncasecmp(u32 a, u32 b, u32 n) { return casecmp(str(a), str(b), n); }
int g_strcoll(u32 a, u32 b) { return std::strcmp(str(a), str(b)); }
u32 g_strxfrm(u32 d, u32 s, u32 n) {
    u32 len = u32(std::strlen(str(s)));
    if (n) { std::strncpy(ptr(d), str(s), n - 1); ptr(d)[n - 1] = 0; }
    return len;
}
u32 g_strcpy(u32 d, u32 s) { std::strcpy(ptr(d), str(s)); return d; }
u32 g_strncpy(u32 d, u32 s, u32 n) { std::strncpy(ptr(d), str(s), n); return d; }
u32 g_strcat(u32 d, u32 s) { std::strcat(ptr(d), str(s)); return d; }
u32 g_strncat(u32 d, u32 s, u32 n) { std::strncat(ptr(d), str(s), n); return d; }
u32 g_strlcpy(u32 d, u32 s, u32 n) {
    u32 len = u32(std::strlen(str(s)));
    if (n) { u32 k = std::min(len, n - 1); std::memcpy(ptr(d), str(s), k); ptr(d)[k] = 0; }
    return len;
}
u32 g_strlcat(u32 d, u32 s, u32 n) {
    u32 dl = u32(strnlen(str(d), n)), sl = u32(std::strlen(str(s)));
    if (dl == n) return n + sl;
    u32 k = std::min(sl, n - dl - 1);
    std::memcpy(ptr(d) + dl, str(s), k);
    ptr(d)[dl + k] = 0;
    return dl + sl;
}
u32 g_strchr(u32 s, int c) { const char *p = std::strchr(str(s), c); return p ? gaddr(p) : 0; }
u32 g_strrchr(u32 s, int c) { const char *p = std::strrchr(str(s), c); return p ? gaddr(p) : 0; }
u32 g_strstr(u32 h, u32 n) { const char *p = std::strstr(str(h), str(n)); return p ? gaddr(p) : 0; }
u32 g_strpbrk(u32 s, u32 a) { const char *p = std::strpbrk(str(s), str(a)); return p ? gaddr(p) : 0; }
u32 g_strspn(u32 s, u32 a) { return u32(std::strspn(str(s), str(a))); }
u32 g_strcspn(u32 s, u32 a) { return u32(std::strcspn(str(s), str(a))); }
u32 g_strdup(u32 s) { return gstrdup(str(s)); }

u32 strtok_impl(u32 s, u32 delim, u32 *save) {
    if (!s) s = *save;
    if (!s) return 0;
    s += u32(std::strspn(str(s), str(delim)));
    if (!*str(s)) { *save = 0; return 0; }
    u32 end = s + u32(std::strcspn(str(s), str(delim)));
    if (*str(end)) { *ptr(end) = 0; *save = end + 1; }
    else *save = 0;
    return s;
}
u32 g_strtok(u32 s, u32 d) {
    static RT_THREAD_LOCAL(u32, save);
    return strtok_impl(s, d, &RT_TLS(save));
}
u32 g_strtok_r(u32 s, u32 d, u32 savep) { u32 save = LD32(savep); u32 r = strtok_impl(s, d, &save); ST32(savep, save); return r; }

// ---- number parsing ---------------------------------------------------------------------
template <class T, class F>
T parse(u32 s, u32 endp, F fn) {
    char *end = nullptr;
    errno = 0;
    T v = fn(str(s), &end);
    if (errno == ERANGE) set_errno(kERANGE);
    if (endp) ST32(endp, s + u32(end - str(s)));
    return v;
}
int32_t g_strtol(u32 s, u32 e, int base) {
    // bionic long is 32-bit: saturate like strtol would
    long long v = parse<long long>(s, e, [base](const char *p, char **end) { return std::strtoll(p, end, base); });
    if (v > INT32_MAX) { set_errno(kERANGE); return INT32_MAX; }
    if (v < INT32_MIN) { set_errno(kERANGE); return INT32_MIN; }
    return int32_t(v);
}
u32 g_strtoul(u32 s, u32 e, int base) {
    unsigned long long v = parse<unsigned long long>(s, e, [base](const char *p, char **end) { return std::strtoull(p, end, base); });
    const char *p = str(s);
    while (std::isspace(uint8_t(*p))) p++;
    if (*p == '-') return u32(0u - u32(0ull - v));  // strtoul semantics for negative input
    if (v > 0xffffffffull) { set_errno(kERANGE); return 0xffffffffu; }
    return u32(v);
}
int64_t g_strtoll(u32 s, u32 e, int base) {
    return parse<long long>(s, e, [base](const char *p, char **end) { return std::strtoll(p, end, base); });
}
double g_strtod(u32 s, u32 e) { return parse<double>(s, e, [](const char *p, char **end) { return std::strtod(p, end); }); }
int g_atoi(u32 s) { return std::atoi(str(s)); }
int32_t g_atol(u32 s) { return int32_t(std::atol(str(s))); }

// ---- ctype (C locale) --------------------------------------------------------------------
int c_isalnum(int c) { return c >= 0 && c < 128 && std::isalnum(c); }
int c_isalpha(int c) { return c >= 0 && c < 128 && std::isalpha(c); }
int c_iscntrl(int c) { return c >= 0 && c < 128 && std::iscntrl(c); }
int c_islower(int c) { return c >= 'a' && c <= 'z'; }
int c_isupper(int c) { return c >= 'A' && c <= 'Z'; }
int c_isprint(int c) { return c >= 32 && c < 127; }
int c_ispunct(int c) { return c >= 0 && c < 128 && std::ispunct(c); }
int c_isxdigit(int c) { return c >= 0 && c < 128 && std::isxdigit(c); }
int c_tolower(int c) { return c_isupper(c) ? c + 32 : c; }
int c_toupper(int c) { return c_islower(c) ? c - 32 : c; }

// bionic _ctype_ flag bits
enum : uint8_t { kU = 0x01, kL = 0x02, kN = 0x04, kS = 0x08, kP = 0x10, kC = 0x20, kX = 0x40, kB = 0x80 };

uint32_t init_ctype(const char *) {
    // _ctype_ is `const char *`; the variable points at a 1+256 byte table (index 0 = EOF).
    uint32_t var = gcalloc(1, 8 + 257);
    uint32_t table = var + 8;
    for (int c = 0; c < 128; c++) {
        uint8_t f = 0;
        if (std::isupper(c)) f |= kU;
        if (std::islower(c)) f |= kL;
        if (std::isdigit(c)) f |= kN;
        if (std::isspace(c)) f |= kS;
        if (std::ispunct(c)) f |= kP;
        if (std::iscntrl(c)) f |= kC;
        if (std::isxdigit(c) && !std::isdigit(c)) f |= kX;
        if (c == ' ') f |= kB;
        ST8(table + 1 + c, f);
    }
    ST32(var, table);
    return var;
}
uint32_t init_tolower_tab(const char *) {
    // `const short *_tolower_tab_` -> 1+256 shorts, index 0 = EOF (-1)
    uint32_t var = gcalloc(1, 8 + 257 * 2);
    uint32_t table = var + 8;
    ST16(table, 0xffff);
    for (int c = 0; c < 256; c++) ST16(table + 2 + 2 * c, u32(c_tolower(c)));
    ST32(var, table);
    return var;
}

// ---- wide characters (32-bit wchar_t, C locale: bytes map 1:1) ------------------------------
u32 w_wcslen(u32 s) { u32 n = 0; while (LD32(s + 4 * n)) n++; return n; }
int w_wcscmp(u32 a, u32 b) {
    for (;; a += 4, b += 4) {
        int32_t x = int32_t(LD32(a)), y = int32_t(LD32(b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
int w_wcscoll(u32 a, u32 b) { return w_wcscmp(a, b); }
u32 w_wcscpy(u32 d, u32 s) { u32 r = d; do { ST32(d, LD32(s)); d += 4; s += 4; } while (LD32(s - 4)); return r; }
u32 w_wcsxfrm(u32 d, u32 s, u32 n) {
    u32 len = w_wcslen(s);
    for (u32 i = 0; i < n; i++) { ST32(d + 4 * i, i < len ? LD32(s + 4 * i) : 0); if (i >= len) break; }
    return len;
}
u32 w_wmemcpy(u32 d, u32 s, u32 n) { std::memcpy(ptr(d), ptr(s), size_t(n) * 4); return d; }
u32 w_wmemmove(u32 d, u32 s, u32 n) { std::memmove(ptr(d), ptr(s), size_t(n) * 4); return d; }
u32 w_wmemset(u32 d, u32 c, u32 n) { for (u32 i = 0; i < n; i++) ST32(d + 4 * i, c); return d; }
u32 w_wmemchr(u32 s, u32 c, u32 n) { for (u32 i = 0; i < n; i++) if (LD32(s + 4 * i) == c) return s + 4 * i; return 0; }
int w_wmemcmp(u32 a, u32 b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        int32_t x = int32_t(LD32(a + 4 * i)), y = int32_t(LD32(b + 4 * i));
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
u32 w_btowc(int c) { return c < 0 || c > 255 ? 0xffffffffu : u32(c); }
int w_wctob(u32 c) { return c < 256 ? int(c) : -1; }
u32 w_mbrtowc(u32 pwc, u32 s, u32 n, u32) {
    if (!s) return 0;
    if (!n) return 0xfffffffeu;
    uint8_t c = LD8(s);
    if (pwc) ST32(pwc, c);
    return c ? 1 : 0;
}
u32 w_wcrtomb(u32 s, u32 wc, u32) {
    if (!s) return 1;
    if (wc > 255) { set_errno(84); return 0xffffffffu; }  // EILSEQ
    ST8(s, uint8_t(wc));
    return 1;
}
u32 w_mbstowcs(u32 d, u32 s, u32 n) {
    u32 len = u32(std::strlen(str(s)));
    if (!d) return len;
    u32 i = 0;
    for (; i < n && i < len; i++) ST32(d + 4 * i, LD8(s + i));
    if (i < n) ST32(d + 4 * i, 0);
    return i;
}
u32 w_towlower(u32 c) { return c < 128 ? u32(c_tolower(int(c))) : c; }
u32 w_towupper(u32 c) { return c < 128 ? u32(c_toupper(int(c))) : c; }

// wctype(): return a small id per class name; iswctype tests it with the C-locale rules.
const char *const kWctypeNames[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                    "lower", "print", "punct", "space", "upper", "xdigit"};
u32 w_wctype(u32 name) {
    for (u32 i = 1; i < std::size(kWctypeNames); i++)
        if (!std::strcmp(str(name), kWctypeNames[i])) return i;
    return 0;
}
int w_iswctype(u32 c, u32 t) {
    if (c >= 128) return 0;
    int ch = int(c);
    switch (t) {
    case 1: return std::isalnum(ch) != 0;
    case 2: return std::isalpha(ch) != 0;
    case 3: return ch == ' ' || ch == '\t';
    case 4: return std::iscntrl(ch) != 0;
    case 5: return std::isdigit(ch) != 0;
    case 6: return std::isgraph(ch) != 0;
    case 7: return std::islower(ch) != 0;
    case 8: return std::isprint(ch) != 0;
    case 9: return std::ispunct(ch) != 0;
    case 10: return std::isspace(ch) != 0;
    case 11: return std::isupper(ch) != 0;
    case 12: return std::isxdigit(ch) != 0;
    }
    return 0;
}

// ---- strerror (Linux errno numbering) ------------------------------------------------------
std::string error_text(int e) {
    switch (e) {
    case 0: return "Success";
    case kEPERM: return "Operation not permitted";
    case kENOENT: return "No such file or directory";
    case kEINTR: return "Interrupted system call";
    case kEIO: return "I/O error";
    case kEBADF: return "Bad file descriptor";
    case kEAGAIN: return "Try again";
    case kENOMEM: return "Out of memory";
    case kEACCES: return "Permission denied";
    case kEEXIST: return "File exists";
    case kENOTDIR: return "Not a directory";
    case kEISDIR: return "Is a directory";
    case kEINVAL: return "Invalid argument";
    case kENOSPC: return "No space left on device";
    case kENOSYS: return "Function not implemented";
    case kENETUNREACH: return "Network is unreachable";
    case kECONNREFUSED: return "Connection refused";
    case kETIMEDOUT: return "Connection timed out";
    }
    return "Unknown error " + std::to_string(e);
}
u32 g_strerror(int e) {
    static std::unordered_map<int, u32> cache;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(e);
    return it != cache.end() ? it->second : cache[e] = gstrdup(error_text(e).c_str());
}
int g_strerror_r(int e, u32 buf, u32 n) {  // XSI variant (bionic): returns 0 or ERANGE
    std::string s = error_text(e);
    if (!n) return kERANGE;
    std::strncpy(ptr(buf), s.c_str(), n - 1);
    ptr(buf)[n - 1] = 0;
    return s.size() < n ? 0 : kERANGE;
}

// ---- qsort / rand48 ------------------------------------------------------------------------
void g_qsort(Cpu *cpu, u32 base, u32 n, u32 size, u32 cmp) {
    if (n < 2 || !size) return;
    // sort an index permutation with the guest comparator, then apply it
    std::vector<u32> idx(n);
    for (u32 i = 0; i < n; i++) idx[i] = i;
    std::vector<uint8_t> copy(ptr<uint8_t>(base), ptr<uint8_t>(base) + size_t(n) * size);
    // the comparator must see elements in place: compare by their original addresses
    std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) {
        return int32_t(call_nested(cpu, cmp, {base + a * size, base + b * size})) < 0;
    });
    for (u32 i = 0; i < n; i++) std::memcpy(ptr(base + i * size), &copy[size_t(idx[i]) * size], size);
}

uint64_t g_rand48 = 0x1234abcd330eull;
void g_srand48(int32_t seed) { g_rand48 = (uint64_t(uint32_t(seed)) << 16) | 0x330e; }
int32_t g_lrand48() {
    g_rand48 = (g_rand48 * 0x5deece66dull + 0xb) & ((1ull << 48) - 1);
    return int32_t(g_rand48 >> 17);
}

}  // namespace

RT_IMPORT(memcpy, g_memcpy)
RT_IMPORT(memmove, g_memmove)
RT_IMPORT(memset, g_memset)
RT_IMPORT(memcmp, g_memcmp)
RT_IMPORT(memchr, g_memchr)
RT_IMPORT(memmem, g_memmem)
RT_IMPORT(strlen, g_strlen)
RT_IMPORT(strcmp, g_strcmp)
RT_IMPORT(strncmp, g_strncmp)
RT_IMPORT(strcasecmp, g_strcasecmp)
RT_IMPORT(strncasecmp, g_strncasecmp)
RT_IMPORT(strcoll, g_strcoll)
RT_IMPORT(strxfrm, g_strxfrm)
RT_IMPORT(strcpy, g_strcpy)
RT_IMPORT(strncpy, g_strncpy)
RT_IMPORT(strcat, g_strcat)
RT_IMPORT(strncat, g_strncat)
RT_IMPORT(strlcpy, g_strlcpy)
RT_IMPORT(strlcat, g_strlcat)
RT_IMPORT(strchr, g_strchr)
RT_IMPORT(strrchr, g_strrchr)
RT_IMPORT(strstr, g_strstr)
RT_IMPORT(strpbrk, g_strpbrk)
RT_IMPORT(strspn, g_strspn)
RT_IMPORT(strcspn, g_strcspn)
RT_IMPORT(strdup, g_strdup)
RT_IMPORT(strtok, g_strtok)
RT_IMPORT(strtok_r, g_strtok_r)
RT_IMPORT(strtol, g_strtol)
RT_IMPORT(strtoul, g_strtoul)
RT_IMPORT(strtoll, g_strtoll)
RT_IMPORT(strtod, g_strtod)
RT_IMPORT(atoi, g_atoi)
RT_IMPORT(atol, g_atol)
RT_IMPORT(isalnum, c_isalnum)
RT_IMPORT(isalpha, c_isalpha)
RT_IMPORT(iscntrl, c_iscntrl)
RT_IMPORT(islower, c_islower)
RT_IMPORT(isupper, c_isupper)
RT_IMPORT(isprint, c_isprint)
RT_IMPORT(ispunct, c_ispunct)
RT_IMPORT(isxdigit, c_isxdigit)
RT_IMPORT(tolower, c_tolower)
RT_IMPORT(toupper, c_toupper)
RT_DATA(_ctype_, init_ctype)
RT_DATA(_tolower_tab_, init_tolower_tab)
RT_IMPORT(wcslen, w_wcslen)
RT_IMPORT(wcscmp, w_wcscmp)
RT_IMPORT(wcscoll, w_wcscoll)
RT_IMPORT(wcscpy, w_wcscpy)
RT_IMPORT(wcsxfrm, w_wcsxfrm)
RT_IMPORT(wmemcpy, w_wmemcpy)
RT_IMPORT(wmemmove, w_wmemmove)
RT_IMPORT(wmemset, w_wmemset)
RT_IMPORT(wmemchr, w_wmemchr)
RT_IMPORT(wmemcmp, w_wmemcmp)
RT_IMPORT(btowc, w_btowc)
RT_IMPORT(wctob, w_wctob)
RT_IMPORT(mbrtowc, w_mbrtowc)
RT_IMPORT(wcrtomb, w_wcrtomb)
RT_IMPORT(mbstowcs, w_mbstowcs)
RT_IMPORT(towlower, w_towlower)
RT_IMPORT(towupper, w_towupper)
RT_IMPORT(wctype, w_wctype)
RT_IMPORT(iswctype, w_iswctype)
RT_IMPORT(strerror, g_strerror)
RT_IMPORT(strerror_r, g_strerror_r)
RT_IMPORT(qsort, g_qsort)
RT_IMPORT(srand48, g_srand48)
RT_IMPORT(lrand48, g_lrand48)

}  // namespace rt
