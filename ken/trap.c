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
    callp = &sysent[u.u_ar0[R3]];

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
