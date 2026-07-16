#include "os.h"

/*
 * Call the system-entry routine f (out of the
 * sysent table). This is a subroutine for trap, and
 * not in-line, because if a signal occurs
 * during processing, an (abnormal) return is simulated from
 * the last caller to savu(qsav); if this took place
 * inside of trap, it wouldn't have a chance to clean up.
 *
 * If this occurs, the return takes place without
 * clearing u_intflg; if it's still set, trap
 * marks an error which means that a system
 * call (like read on a typewriter) got interrupted
 * by a signal.
 */
void trap1(void (*f)())
{
    u.u_intflg = 1;
    if (save(u.u_qsav)) {
        return;
    }
    (*f)();
    u.u_intflg = 0;
}

/*
 * nonexistent system call-- set fatal error code.
 */
void nosys(void)
{
    u.u_error = 100;
}

/*
 * Ignored system call
 */
void nullsys(void)
{
}

void trap_epilogue(void)
{
    struct ctx far *ctx;
    ctx = (struct ctx far *)MK_FP(u.u_stack[KSSIZE - 1], u.u_stack[KSSIZE - 2]);
    ctx->ax = u.u_ar0[R0];
    ctx->bx = u.u_ar0[R1];
    ctx->cx = u.u_ar0[R2];
    ctx->dx = u.u_ar0[R3];
}

void trap(void)
{
    register struct sysent *callp;
    callp = &sysent[u.u_ar0[R3] & 077];

    u.u_dirp = u.u_arg[0];
    trap1(callp->call);
    if(u.u_intflg)
        u.u_error = EINTR;   

    if(u.u_error) {
        u.u_ar0[R0] = -u.u_error;
    }
    u.u_ar0[R3] = u.u_error;
    trap_epilogue();

    if(issig())
        psig();
    setpri(u.u_procp);
}

void trap0(int ds, int es, int dx, int cx, int bx, int ax,
    int di, int si, int bp, int ip, int cs, int flags,
    int arg0, int arg1, int arg2)
{
    (void)es; (void)ds; (void)si; (void)di; (void)bp;
    (void)ip; (void)cs; (void)flags;

    u.u_ar0[R0] = ax;
    u.u_ar0[R1] = bx;
    u.u_ar0[R2] = cx;
    u.u_ar0[R3] = dx;
    u.u_arg[0] = arg0;
    u.u_arg[1] = arg1;
    u.u_arg[2] = arg2;
    u.u_dirp = u.u_arg[0];
    u.u_error = 0;
}

/*
 * The VMM u-area window is exactly one page; the kernel stack lives at its
 * top (U_AREA + 1024 = 0xD400).  segflt/psig/trap_epilogue depend on that
 * layout, so pin the size at compile time.
 */
typedef char user_size_assert[sizeof(struct user) == 0x400 ? 1 : -1];

/*
 * Copy the faulting 12-word interrupt frame (struct ctx) that _segflt_isr
 * built on the kernel stack down onto the user stack at usp-24, and point the
 * return SS:SP (u_stack[KSSIZE-1:-2]) at it.  On return _segflt_isr reloads
 * that SS:SP and IRETs through the frame: for a restart this re-runs the
 * faulting instruction; for a caught signal psig() first duplicates the frame
 * once more and vectors the trampoline.  Both need the frame on the user
 * stack because a V86 IRET does not reload SS:SP.
 */
static void dupframe(unsigned kctx, unsigned uss, unsigned usp)
{
    int far *dst = (int far *)MK_FP(uss, usp - 24);
    int *src = (int *)kctx;            /* near: the ctx in the u page (DS-relative) */
    int i;

    for(i = 0; i < 12; i++)
        dst[i] = src[i];
    u.u_stack[KSSIZE - 1] = uss;
    u.u_stack[KSSIZE - 2] = usp - 24;
}

/*
 * segflt -- kernel handler for a VMM-reflected user #PF, the x86 stand-in for
 * the PDP-11 segmentation exception (V6 trap.c case 9).  Entered on the kernel
 * stack from _segflt_isr (dmr/m86.asm) with IF=0; kctx is the near address of
 * the faulting frame, usp/uss the faulting user SP/SS, fa the WIN_DATA offset
 * of the fault (or >= USTACK for a read-only text write).
 *
 * If the user SP has moved below the stack, grow the stack to cover it and
 * re-execute the faulting instruction; otherwise it is a segmentation
 * violation (SIGSEG, default core+exit, catchable on the user stack).
 *
 * grow(usp-24): the x86 leaves SP un-decremented on a faulting push, so a
 * push landing on the stack bottom faults with sp==bottom; growing 24 bytes
 * (one ctx) of headroom covers both that word and the restart frame dupframe
 * writes at usp-24.
 */
void segflt(unsigned fa, unsigned usp, unsigned uss, unsigned kctx)
{
    struct ctx *k = (struct ctx *)kctx;
    int n;

    spl0();      /* allow clock; grow may sleep in expand's swap path */

    u.u_ar0[R0] = k->ax;
    u.u_ar0[R1] = k->bx;
    u.u_ar0[R2] = k->cx;
    u.u_ar0[R3] = k->dx;

    if(fa < (unsigned)USTACK && grow(usp - 24)) {
        dupframe(kctx, uss, usp);            /* restart on the grown stack */
        goto out;
    }
    psignal(u.u_procp, SIGSEG);
    if(n = issig()) {
        if(u.u_signal[n] != 0) {             /* caught: deliver on the user stack */
            dupframe(kctx, uss, usp);
            psig();
            goto out;
        }
        psig();                              /* default: core + exit, no return */
    }
    dupframe(kctx, uss, usp);                /* ignored signal: restart (will recur) */
out:
    setpri(u.u_procp);
}
