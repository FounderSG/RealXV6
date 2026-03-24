#include "os.h"

/*
 * Give up the processor till a wakeup occurs
 * on chan, at which time the process
 * enters the scheduling queue at priority pri.
 * The most important effect of pri is that when
 * pri<0 a signal cannot disturb the sleep;
 * if pri>=0 signals will be processed.
 * Callers of this routine must be prepared for
 * premature return, and check that the reason for
 * sleeping has gone away.
 */
void sleep(void *chan, int pri)
{
    register struct proc *rp;
    int s;

    s = getps();
    rp = u.u_procp;
    if(pri >= 0) {
        if(issig())
            goto psig;
        spl6();
        rp->p_wchan = chan;
        rp->p_stat = SWAIT;
        rp->p_pri = pri;
        spl0();
        if(runin != 0) {
            runin = 0;
            wakeup(&runin);
        }
        swtch();
        if(issig())
            goto psig;
    } else {
        spl6();
        rp->p_wchan = chan;
        rp->p_stat = SSLEEP;
        rp->p_pri = pri;
        spl0();
        swtch();
    }
    setps(s);
    return;

    /*
     * If priority was low (>=0) and
     * there has been a signal,
     * execute non-local goto to
     * the qsav location.
     * (see trap1/trap.c)
     */
psig:
    resume(u.u_procp, u.u_qsav);
}

/*
 * Wake up all processes sleeping on chan.
 */
void wakeup(void *chan)
{
    register struct proc *p;
    register int c, i;

    c = chan;
    p = &proc[0];
    i = NPROC;
    do {
        if(p->p_wchan == c) {
            setrun(p);
        }
        p++;
    } while(--i);
}

/*
 * Set the process running;
 * arrange for it to be swapped in if necessary.
 */
void setrun(struct proc *p)
{
    register struct proc *rp;

    rp = p;
    rp->p_wchan = 0;
    rp->p_stat = SRUN;
    if(rp->p_pri < curpri)
        runrun++;
    if(runout != 0 && (rp->p_flag&SLOAD) == 0) {
        runout = 0;
        wakeup(&runout);
    }
}

/*
 * Set user priority.
 * The rescheduling flag (runrun)
 * is set if the priority is higher
 * than the currently running process.
 */
void setpri(struct proc *up)
{
    register struct proc *pp;
    register int p;

    pp = up;
    p = (pp->p_cpu & 0377)/16;
    p += PUSER + pp->p_nice;
    if(p > 127)
        p = 127;
    if(p < curpri)
        runrun++;
    pp->p_pri = p;
}

/*
 * The main loop of the scheduling (swapping)
 * process.
 * The basic idea is:
 *  see if anyone wants to be swapped in;
 *  swap out processes until there is room;
 *  swap him in;
 *  repeat.
 * Although it is not remarkably evident, the basic
 * synchronization here is on the runin flag, which is
 * slept on and is set once per second by the clock routine.
 * Core shuffling therefore takes place once per second.
 *
 * panic: swap error -- IO error while swapping.
 *  this is the one panic that should be
 *  handled in a less drastic way. Its
 *  very hard.
 */
void sched(void)
{
    struct proc *p1;
    register struct proc *rp;
    register int a, n;

    /*
     * find user to swap in
     * of users ready, select one out longest
     */

    goto loop;

sloop:
    runin++;
    sleep(&runin, PSWP);

loop:
    spl6();
    n = -1;
    for(rp = &proc[0]; rp < &proc[NPROC]; rp++)
    if(rp->p_stat==SRUN && (rp->p_flag&SLOAD)==0 &&
        rp->p_time > n) {
        p1 = rp;
        n = rp->p_time;
    }
    if(n == -1) {
        runout++;
        sleep(&runout, PSWP);
        goto loop;
    }

    /*
     * see if there is core for that process
     */

    spl0();
    rp = p1;
    a = rp->p_size;
    if((a=malloc(coremap, a)) != NULL)
        goto found2;

    /*
     * none found,
     * look around for easy core
     */

    spl6();
    for(rp = &proc[0]; rp < &proc[NPROC]; rp++)
    if((rp->p_flag&(SSYS|SLOCK|SLOAD))==SLOAD &&
        (rp->p_stat == SWAIT || rp->p_stat==SSTOP))
        goto found1;

    /*
     * no easy core,
     * if this process is deserving,
     * look around for
     * oldest process in core
     */

    if(n < 3)
        goto sloop;
    n = -1;
    for(rp = &proc[0]; rp < &proc[NPROC]; rp++)
    if((rp->p_flag&(SSYS|SLOCK|SLOAD))==SLOAD &&
       (rp->p_stat==SRUN || rp->p_stat==SSLEEP) &&
        rp->p_time > n) {
        p1 = rp;
        n = rp->p_time;
    }
    if(n < 2)
        goto sloop;
    rp = p1;

    /*
     * swap user out
     */

found1:
    a = malloc(swapmap, rp->p_size*(PAGESIZ/512));
    if(a == NULL) goto sloop;
    spl0();
    rp->p_flag &= ~SLOAD;
    xswap(rp, 1, a);
    goto loop;

    /*
     * swap user in
     */

found2:
    rp = p1;
    if(swap(rp->p_addr, a, rp->p_size, B_READ))
        goto swaper;
    mfree(swapmap, rp->p_size*(PAGESIZ/512), rp->p_addr);
    wakeup(&swapmap);
    rp->p_addr = a;
    rp->p_flag |= SLOAD;
    rp->p_time = 0;
    estabur(rp->p_addr);
    goto loop;

swaper:
    panic("swap error");
}

/*
 * This routine is called to reschedule the CPU.
 * if the calling process is not in RUN state,
 * arrangements for it to restart must have
 * been made elsewhere, usually by calling via sleep.
 */
void swtch(void)
{
    static struct proc *p;
    register int i, n;
    register struct proc *rp;

    /*
     * If not the idle process, resume the idle process.
     */
    if (u.u_procp != &proc[0]) {
        if (save(u.u_rsav)) {
            return;
        }
        resume(&proc[0], u.u_qsav);
    }

    if(p == NULL)
        p = &proc[0];

    /*
     * The first save returns nonzero when proc 0 is resumed
     * by another process (above); then the second is not done
     * and the process-search loop is entered.
     *
     * The first save returns 0 when swtch is called in proc 0
     * from sched().  The second save returns 0 immediately, so
     * in this case too the process-search loop is entered.
     * Thus when proc 0 is awakened by being made runnable, it will
     * find itself and resume itself at rsav, and return to sched().
     */
    if (save(u.u_qsav)==0 && save(u.u_rsav))
        return;

loop:
    runrun = 0;
    rp = p;
    p = NULL;
    n = 128;
    /*
     * Search for highest-priority runnable process
     */
    i = NPROC;
    do {
        rp++;
        if(rp >= &proc[NPROC])
            rp = &proc[0];
        if(rp->p_stat==SRUN && (rp->p_flag&SLOAD)!=0) {
            if(rp->p_pri < n) {
                p = rp;
                n = rp->p_pri;
            }
        }
    } while(--i);
    /*
     * If no process is runnable, idle.
     */
    if(p == NULL) {
        p = rp;
        idle();
        goto loop;
    }
    rp = p;
    curpri = n;
    /*
     * The rsav (ssav) contents are interpreted in the new address space
     * You are not expected to understand this.
     */
    n = p->p_flag&SSWAP;
    p->p_flag &= ~SSWAP;
    resume(p, n? u.u_ssav: u.u_rsav);
}

/*
 * Create a new process-- the internal version of
 * sys fork.
 * It returns 1 in the new process.
 * How this happens is rather hard to understand.
 * The essential fact is that the new process is created
 * in such a way that appears to have started executing
 * in the same call to newproc as the parent;
 * but in fact the code that runs is that of swtch.
 * The subtle implication of the returned value of swtch
 * (see above) is that this is the value that newproc's
 * caller in the new process sees.
 */
int newproc(void)
{
    uint a1, a2;
    struct proc *p, *up;
    register struct proc *rpp, *rip;
    register int *rfp;
    register struct file *ofp;
    register int n;

    p = NULL;
    /*
     * First, just locate a slot for a process
     * and copy the useful info from this process into it.
     * The panic "cannot happen" because fork has already
     * checked for the existence of a slot.
     */
retry:
    mpid++;
    if(mpid < 0) {
        mpid = 0;
        goto retry;
    }
    for(rpp = &proc[0]; rpp < &proc[NPROC]; rpp++) {
        if(rpp->p_stat == NULL && p==NULL)
            p = rpp;
        if (rpp->p_pid==mpid)
            goto retry;
    }
    if ((rpp = p)==NULL)
        panic("no procs");

    /*
     * make proc entry for new proc
     */

    rip = u.u_procp;
    up = rip;
    rpp->p_stat = SRUN;
    rpp->p_flag = SLOAD;
    rpp->p_uid = rip->p_uid;
    rpp->p_ttyp = rip->p_ttyp;
    rpp->p_nice = rip->p_nice;
    rpp->p_textp = rip->p_textp;
    rpp->p_pid = mpid;
    rpp->p_ppid = rip->p_pid;
    rpp->p_time = 0;

    /*
     * make duplicate entries
     * where needed
     */

    for(rfp = &u.u_ofile[0]; rfp < &u.u_ofile[NOFILE];)
        if((ofp = *rfp++) != NULL)
            ofp->f_count++;
    u.u_cdir->i_count++;
    /*
     * Partially simulate the environment
     * of the new process so that when it is actually
     * created (by copying) it will look right.
     */
    rpp = p;
    u.u_procp = rpp;
    rip = up;
    if (save(u.u_rsav)) {
        return(1);
    }
    savu(rip);
    n = rip->p_size;
    a1 = rip->p_addr;
    rpp->p_size = n;
    a2 = malloc(coremap, n);
    /*
     * If there is not enough core for the
     * new process, swap out the current process to generate the
     * copy.
     */
    if(a2 == NULL) {
        rip->p_stat = SIDL;
        rpp->p_addr = a1;
        if (save(u.u_ssav)) {
            return(1);
        }
        savu(rip);
        while(1) {
            a2 = malloc(swapmap, rpp->p_size*(PAGESIZ/512));
            if(a2 != NULL) break;
            sleep(&swapmap, PSWP);
        };
        xswap(rpp, 0, a2);
        rpp->p_flag |= SSWAP;
        rip->p_stat = SRUN;
    } else {
    /*
     * There is core, so just copy.
     */
        rpp->p_addr = a2;
        while(n--)
            copyseg(a1++, a2++);
        estabur(rpp->p_addr);
    }
    u.u_procp = rip;
    return(0);
}

/* external helper function defined in asm code */
extern void use_resume_stack(void);
extern void do_resume(label_t ctx);
struct proc *resume_proc;
int *resume_ctx;
void resume(struct proc *p, label_t ctx)
{
    resume_proc = p;
    resume_ctx = ctx;
    use_resume_stack();

    if(resume_proc != u.u_procp)
    {
        if(u.u_procp->p_stat != SZOMB)
            savu(u.u_procp);
        retu(resume_proc);
    }
    do_resume(resume_ctx);
}

void estabur(uint addr)
{
    struct user far *pu;
    struct ctx far *ctx;

    addr = addr * (PAGESIZ / 16);
    pu = (struct user far *)MK_FP(addr, USTACK);
    pu->u_stack[KSSIZE - 1] = addr;    /* SS */
    ctx = (struct ctx far *)MK_FP(addr, pu->u_stack[KSSIZE - 2]);
    ctx->cs = addr;
    ctx->ds = addr;
    ctx->es = addr;
}
