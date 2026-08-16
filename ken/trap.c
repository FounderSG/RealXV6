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

/*
 * privflt -- kernel handler for a VMM-reflected user-mode privileged/illegal
 * operation, the x86 stand-in for the PDP-11 illegal-instruction trap (V6
 * trap.c cases 1/2/8/9-ish).  Entered on the kernel stack from _privflt_isr
 * (dmr/m86.asm) with IF=0, mirroring segflt(): kctx is the near address of the
 * faulting frame, usp/uss the faulting user SP/SS, and type the trap-type code
 * the VMM stashed (always 1 = privileged op -> SIGINS).
 *
 * Unlike segflt there is no grow() branch: a privileged op is never a
 * recoverable stack fault.  We post the signal and run the same issig/psig
 * dance -- a caught handler is delivered on the user stack via dupframe; the
 * default action (core+exit) never returns.  An ignored SIGINS falls through
 * to a restart, which re-executes the faulting instruction and re-faults
 * (PDP-11 re-execution semantics).
 */
void privflt(unsigned type, unsigned usp, unsigned uss, unsigned kctx)
{
    struct ctx *k = (struct ctx *)kctx;
    int n, sig;

    spl0();

    u.u_ar0[R0] = k->ax;
    u.u_ar0[R1] = k->bx;
    u.u_ar0[R2] = k->cx;
    u.u_ar0[R3] = k->dx;

    switch(type) {                           /* VMM trap-type code -> V6 signal */
    case 2:  sig = SIGTRC; break;            /* breakpoint (int3) */
    case 3:  sig = SIGFPT; break;            /* arithmetic (divide / overflow) */
    case 1:                                  /* privileged operation */
    case 4:                                  /* illegal instruction (#UD) */
    default: sig = SIGINS; break;
    }

    psignal(u.u_procp, sig);
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
