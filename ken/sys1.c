#include "os.h"

/*
 * exec system call.
 * Because of the fact that an I/O buffer is used
 * to store the caller's arguments during exec,
 * and more buffers are needed to read in the text file,
 * deadly embraces waiting for free buffers are possible.
 * Therefore the number of processes simultaneously
 * running in exec has to be limited to NEXEC.
 */
#define EXPRI   -1

void exec(void)
{
    int ap, na, nc;
    int ts, ds;
    struct buf *bp;
    struct inode *ip;
    register int c;
    register char *cp;
    register int *wp;

    /*
     * pick up file names
     * and check various modes
     * for execute permission
     */

    ip = namei(&uchar, 0);
    if(ip == NULL)
        return;
    while(execnt >= NEXEC)
        sleep(&execnt, EXPRI);
    execnt++;
    bp = getblk(NODEV, 0);
    if(access(ip, IEXEC) || (ip->i_mode&IFMT)!=0)
        goto bad;

    /*
     * pack up arguments into
     * allocated disk buffer
     */
    cp = bp->b_addr;
    na = 0;
    nc = 0;
    while(ap = fuword(u.u_arg[1])) {
        na++;
        if(ap == -1)
            goto bad;
        u.u_arg[1] += 2;
        for(;;) {
            c = fubyte(ap++);
            if(c == -1)
                goto bad;
            *cp++ = c;
            nc++;
            if(nc > 510) {
                u.u_error = E2BIG;
                goto bad;
            }
            if(c == 0)
                break;
        }
    }
    if((nc&1) != 0) {
        *cp++ = 0;
        nc++;
    }

    /*
     * read in first 8 bytes of file
     * w0 = JMP Instruction
     * w1 = 0x6146, magic number
     * w2 = 0x676e, magic number
     * w3 = JMP Instruction
     */

    u.u_base = (char *)&u.u_arg[0];
    u.u_count = 8;
    u.u_offset[1] = 0;
    u.u_offset[0] = 0;
    u.u_segflg = 1;
    readi(ip);
    u.u_segflg = 0;
    if(u.u_error)
        goto bad;
    if(u.u_arg[1] != 0x6146 || u.u_arg[2] != 0x676e) {
        u.u_error = ENOEXEC;
        goto bad;
    }

    for(c=0; c<USIZE; c++)
        clearseg(u.u_procp->p_addr+c);
    u.u_base = (char *)0x100;
    u.u_offset[1] = 0;
    u.u_count = ip->i_size1;
    u.u_segflg = 0;
    readi(ip);

    /*
     * initialize stack segment
     */
    cp = bp->b_addr;
    /* argc, argv, [arg0, arg1, .., 0] [strings] */
    /*
     * The top word of the stack is left unused so the SP holder below it
     * sits at a fixed offset in the 512-byte frame image psinfo hands ps.
     */
    ap = (USTACK-4) - nc - na*2 - 6;
    suword(USTACK-4, ap);        /* user sp */
    ts = ap - 24;
    suword(ap, na);              /* argc */
    suword(ap + 2, ap + 4);      /* argv */
    ap += 4;
    c = (USTACK-4) - nc;
    while(na--) {
        suword(ap, c);
        ap += 2;
        do
            subyte(c++, *cp);
        while(*cp++);
    }
    suword(ap, 0);               /* argv[argc] = NULL; exec's own arg walk
                                  * (and any program passing main's argv on)
                                  * relies on the vector being terminated */

    ds = u.u_procp->p_addr*(PAGESIZ/16);
    suword(ts + 0, ds);           /* ds */
    suword(ts + 2, ds);           /* es */
    suword(ts + 18, 0x100);       /* ip */
    suword(ts + 20, ds);          /* cs */
    suword(ts + 22, 0x200);       /* flag */
    u.u_stack[KSSIZE - 2] = ts;   /* sp */

    /*
     * set SUID/SGID protections, if no tracing
     */

    if ((u.u_procp->p_flag&STRC)==0) {
        if(ip->i_mode&ISUID)
            if(u.u_uid != 0) {
                u.u_uid = ip->i_uid;
                u.u_procp->p_uid = ip->i_uid;
            }
        if(ip->i_mode&ISGID)
            u.u_gid = ip->i_gid;
    }

    /*
     * clear sigs, regs and return
     */
    for(wp = &u.u_signal[0]; wp < &u.u_signal[NSIG]; wp++)
        if(*wp != 1)
            *wp = 0;

bad:
    iput(ip);
    brelse(bp);
    if(execnt >= NEXEC)
        wakeup(&execnt);
    execnt--;
}

/*
 * exit system call:
 * pass back caller's r0
 */
void rexit(void)
{
    u.u_arg[0] = u.u_ar0[R0] << 8;
    exit();
}

/*
 * Release resources.
 * Save u. area for parent to look at.
 * Enter zombie state.
 * Wake up parent and init processes,
 * and dispose of children.
 */
void exit(void)
{
    int *w, a;
    struct proc *p, *q;
    struct buf *bp;

    u.u_procp->p_flag &= ~STRC;
    for(w = &u.u_signal[0]; w < &u.u_signal[NSIG];)
        *w++ = 1;
    for(w = &u.u_ofile[0]; w < &u.u_ofile[NOFILE]; w++)
        if(a = *w) {
            *w = NULL;
            closef((struct file *)a);
        }
    iput(u.u_cdir);
    xfree();
    a = malloc(swapmap, 1);
    if(a == NULL)
        panic("out of swap");
    bp = getblk(swapdev, a);
    bcopy(&u, bp->b_addr, 256);
    bwrite(bp);
    q = u.u_procp;
    mfree(coremap, q->p_size, q->p_addr);
    q->p_addr = a;
    q->p_stat = SZOMB;

loop:
    for(p = &proc[0]; p < &proc[NPROC]; p++)
    if(q->p_ppid == p->p_pid) {
        wakeup(&proc[1]);
        wakeup(p);
        for(p = &proc[0]; p < &proc[NPROC]; p++)
        if(q->p_pid == p->p_ppid) {
            p->p_ppid  = 1;
            if (p->p_stat == SSTOP)
                setrun(p);
        }
        swtch();
        /* no return */
    }
    q->p_ppid = 1;
    goto loop;
}

/*
 * Wait system call.
 * Search for a terminated (zombie) child,
 * finally lay it to rest, and collect its status.
 * Look also for stopped (traced) children,
 * and pass back status from them.
 */
void wait(void)
{
    int f;
    struct proc *p;
    struct buf *bp;
    struct user *pu;

    f = 0;

loop:
    for(p = &proc[0]; p < &proc[NPROC]; p++)
    if(p->p_ppid == u.u_procp->p_pid) {
        f++;
        if(p->p_stat == SZOMB) {
            u.u_ar0[R0] = p->p_pid;
            bp = bread(swapdev, f=p->p_addr);
            mfree(swapmap, 1, f);
            p->p_stat = NULL;
            p->p_pid = 0;
            p->p_ppid = 0;
            p->p_sig = 0;
            p->p_ttyp = 0;
            p->p_flag = 0;
            pu = bp->b_addr;
            u.u_cstime[0] += pu->u_cstime[0];
            dpadd(u.u_cstime, pu->u_cstime[1]);
            dpadd(u.u_cstime, pu->u_stime);
            u.u_cutime[0] += pu->u_cutime[0];
            dpadd(u.u_cutime, pu->u_cutime[1]);
            dpadd(u.u_cutime, pu->u_utime);
            u.u_ar0[R1] = pu->u_arg[0];
            brelse(bp);
            return;
        }
        if(p->p_stat == SSTOP) {
            if((p->p_flag&SWTED) == 0) {
                p->p_flag |= SWTED;
                u.u_ar0[R0] = p->p_pid;
                u.u_ar0[R1] = (p->p_sig<<8) | 0177;
                return;
            }
            p->p_flag &= ~(STRC|SWTED);
            setrun(p);
        }
    }
    if(f) {
        sleep(u.u_procp, PWAIT);
        goto loop;
    }
    u.u_error = ECHILD;
}

/*
 * fork system call.
 */
void fork(void)
{
    register struct proc *p1, *p2;

    p1 = u.u_procp;
    for(p2 = &proc[0]; p2 < &proc[NPROC]; p2++)
        if(p2->p_stat == NULL)
            goto found;
    u.u_error = EAGAIN;
    return;

found:
    if(newproc()) {
        u.u_ar0[R0] = 0;
        u.u_ar0[R1] = p1->p_pid;
        u.u_cstime[0] = 0;
        u.u_cstime[1] = 0;
        u.u_stime = 0;
        u.u_cutime[0] = 0;
        u.u_cutime[1] = 0;
        u.u_utime = 0;
        return;
    }
    u.u_ar0[R0] = p2->p_pid;
}

/*
 * break system call.
 *  -- bad planning: "break" is a dirty word in C.
 */
void sbreak(void)
{
}
