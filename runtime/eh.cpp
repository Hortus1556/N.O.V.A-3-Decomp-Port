// C++ exception support for recompiled code.
//
// The game's own libgcc unwinder (recompiled) does all the work: it walks the guest
// stack with .ARM.exidx/.ARM.extab, runs personality routines and finally calls
// restore_core_regs(core) to "jump" to a landing pad. That last step is replaced here:
// every recompiled function that owns landing pads registered a catch frame on entry
// (EH_BEGIN in recomp.h), so we restore the guest registers and longjmp into the
// innermost matching activation, which then dispatches to the landing pad label.
#include "runtime.h"
#include "tls.h"

namespace rt {

static RT_THREAD_LOCAL(RtEhFrame *, t_eh_head);

RtEhFrame *eh_head() { return RT_TLS(t_eh_head); }
void set_eh_head(RtEhFrame *f) { RT_TLS(t_eh_head) = f; }

}  // namespace rt

extern "C" jmp_buf *rt_eh_push(Cpu *cpu, RtEhFrame *f, uint32_t lo, uint32_t hi) {
    f->prev = RT_TLS(rt::t_eh_head);
    f->lo = lo;
    f->hi = hi;
    f->entry_sp = cpu->r[13];
    f->target = 0;
    RT_TLS(rt::t_eh_head) = f;
    return &f->jb;
}

extern "C" void rt_eh_pop(RtEhFrame *f) { RT_TLS(rt::t_eh_head) = f->prev; }

extern "C" void rt_restore_core_regs(Cpu *cpu) {
    uint32_t core = cpu->r[0];
    uint32_t regs[16];
    for (int i = 0; i < 16; i++) regs[i] = LD32(core + 4 * i);
    uint32_t link = (regs[15] & ~1u) - g_load_base, sp = regs[13];
    // Innermost activation of the owning function whose frame lies above the target sp
    // (an inner recursive activation has a lower entry sp than the landing pad's frame).
    for (RtEhFrame *f = RT_TLS(rt::t_eh_head); f; f = f->prev) {
        if (link >= f->lo && link < f->hi && sp < f->entry_sp) {
            for (int i = 0; i < 15; i++) cpu->r[i] = regs[i];
            f->target = link;
            RT_TLS(rt::t_eh_head) = f;
            longjmp(f->jb, 1);
        }
    }
    rt::fatal("C++ exception: landing pad %08x (sp %08x) has no active frame", link, sp);
}
