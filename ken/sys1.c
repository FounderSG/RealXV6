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
    int ts, tp;
    int entry, csb, dsb, ustktop;
    int nblk, ndata, nstack;
    uint da;
    struct text *oldtp, *newtp;
    long foff;
    struct exec hdr;
    struct buf *bp;
    struct inode *ip;
    register int c;
    register char *cp;
    register int *wp;

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
     * Read the executable header (16 bytes).  Only separated I&D (EXE) images
     * (hdr.a_magic == A_MAGIC) are accepted; anything else is ENOEXEC.  The
     * single-segment *execution* mode still exists for the icode bootstrap,
     * which runs identity-mapped until it execs the init EXE.
     */
    u.u_base = (char *)&hdr;
    u.u_count = sizeof(hdr);
    u.u_offset[1] = 0;
    u.u_offset[0] = 0;
    u.u_segflg = 1;
    readi(ip);
    u.u_segflg = 0;
    if(u.u_error)
        goto bad;

    if(hdr.a_magic == A_MAGIC) {
        /*
         * Separated I&D: code runs at WIN_TEXT (CS=WINSEG), data+bss+stack at
         * WIN_DATA (DS=ES=SS=WDSEG), and the u-area lives in its own page
         * (WIN_U) outside both windows.  The data segment is laid out, low to
         * high, as [ data ][ bss ][ gap ][ stack ] with SP pinned at USTACK.
         * The data block is fixed at USTACK/PAGESIZ pages and contiguous, so it
         * maps 1:1 through WIN_DATA (slot N -> physical p_addr+N) and the user
         * stack / SP holder sit at the fixed offset USTACK-2 (0xEFFE) -- where
         * ps finds them via flat /dev/mem, no window awareness needed.  The
         * code segment is attached through the shared text table (xalloc);
         * both the data block and the text attach happen before anything is
         * freed, so a failure leaves the original image intact.
         */
        if((ip->i_flag&ITEXT)==0 && ip->i_count!=1) {
            u.u_error = ETXTBSY;            /* file is open for writing (V6) */
            goto bad;
        }
        ndata = ((unsigned)hdr.a_data + (unsigned)hdr.a_bss) / PAGESIZ;
        if(((unsigned)hdr.a_data + (unsigned)hdr.a_bss) % PAGESIZ)
            ndata++;                        /* data+bss page count (low slots) */
        nstack = (unsigned)hdr.a_stack / PAGESIZ;
        if((unsigned)hdr.a_stack % PAGESIZ)
            nstack++;                       /* stack page count (high slots) */
        nblk = ndata + nstack;             /* compact: data + stack; heap gap unmapped */
        if((unsigned)hdr.a_text == 0) {
            u.u_error = ENOEXEC;            /* an EXE with no text cannot run; also
                                             * keeps p_tsize!=0 <=> p_textp!=NULL */
            goto bad;
        }
        tp = (unsigned)hdr.a_text / PAGESIZ;
        if((unsigned)hdr.a_text % PAGESIZ)
            tp++;                           /* text page count */
        if(estabur(tp, ndata, nstack, 1))   /* try the sizes out (V6) */
            goto bad;

        da = malloc(coremap, nblk);        /* data segment block */
        if(da == NULL)
            da = swgrow(nblk);             /* V6: swap self out; sched evicts
                                            * sleepers and swaps us back in
                                            * with room for old image + da,
                                            * so a failed exec still falls
                                            * back to the intact old image */
        oldtp = u.u_procp->p_textp;        /* xalloc retargets p_textp/p_taddr */
        xalloc(ip, hdr.a_text);            /* attach or build the shared text */
        if(u.u_error) {
            mfree(coremap, nblk, da);      /* nothing attached; old image intact */
            goto bad;
        }

        /*
         * Commit.  Both new blocks are allocated, so freeing the old image now
         * cannot overlap them.  On a single-seg -> EXE transition, RETAIN the
         * old block's page USIZE-1 in place as the EXE u-area's own page: it is
         * the live u-area / kernel stack (windowed at 0xD000), so keeping it
         * put means no copy and no live window remap -- the window already maps
         * 0xD000 there.  A live sureg call here is fatal: its return address
         * would be pushed on the old page but popped from the new one.
         * Only the other (data) pages of the old block are freed.
         */
        if(u.u_procp->p_tsize == 0) {      /* single-seg block is USIZE pages */
            u.u_procp->p_uaddr = u.u_procp->p_addr + (USIZE-1);
            mfree(coremap, USIZE-1, u.u_procp->p_addr);
        } else {                           /* already EXE: keep its u-page, drop old data */
            mfree(coremap, u.u_procp->p_size, u.u_procp->p_addr);
        }
        /*
         * Release the reference to the old text.  V6 exec calls xfree()
         * BEFORE xalloc(); it is deferred to the commit point here so a
         * failed exec keeps the old image.  xfree works on p_textp, which
         * xalloc already retargeted, so swap the old pointer back for the
         * call.  (Same-binary exec: both references land on one entry and
         * the counts still balance.)
         */
        newtp = u.u_procp->p_textp;
        u.u_procp->p_textp = oldtp;
        xfree();
        u.u_procp->p_textp = newtp;
        u.u_procp->p_taddr = newtp->x_caddr;
        u.u_procp->p_tsize = newtp->x_size;
        u.u_procp->p_addr  = da;
        u.u_procp->p_size  = nblk;
        u.u_procp->p_dsize = ndata;

        for(c=0; c<nblk; c++)              /* clear data+bss+stack */
            clearseg(da+c);
        /* the code block was cleared and loaded by xalloc; clearing it
         * here would wipe a segment other processes share */

        /* Install the new image's windows (estabur ends in sureg, as V6).
         * sureg remaps WIN_U to the same page it already maps (p_uaddr is
         * the unchanged u-area page), so this is safe even with the kernel
         * stack live in that page. */
        estabur(tp, ndata, nstack, 1);

        foff = (long)sizeof(hdr) + (unsigned)hdr.a_text;
        u.u_base = (char *)0;              /* load data at DS:0 */
        u.u_offset[0] = (int)(foff >> 16);
        u.u_offset[1] = (int)foff;
        u.u_count = hdr.a_data;
        readi(ip);

        csb = WINSEG;
        dsb = WDSEG;
        entry = hdr.a_entry;
        ustktop = USTACK;                  /* SP holder at USTACK-2 (0xEFFE), fixed for ps */
    } else {
        u.u_error = ENOEXEC;
        goto bad;
    }

    /*
     * initialize stack segment and the initial user interrupt frame
     */
    cp = bp->b_addr;
    /* argc, argv, [arg0, arg1, .., 0] [strings] */
    ap = (ustktop-2) - nc - na*2 - 6;
    suword(ustktop-2, ap);       /* user sp */
    ts = ap - 24;
    suword(ap, na);              /* argc */
    suword(ap + 2, ap + 4);      /* argv */
    ap += 4;
    c = (ustktop-2) - nc;
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

    suword(ts + 0, dsb);          /* ds */
    suword(ts + 2, dsb);          /* es */
    suword(ts + 18, entry);       /* ip */
    suword(ts + 20, csb);         /* cs */
    suword(ts + 22, 0x200);       /* flag */
    u.u_stack[KSSIZE - 1] = dsb;  /* ss */
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
    u.u_arg[0] = u.u_ar0[R0];
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
    mfree(coremap, 1, q->p_uaddr);  /* the private u-page; the shared code
                                     * segment was released by xfree above */
    q->p_tsize = 0;                 /* psinfo/ps key on p_tsize: a zombie must
                                     * not present a stale argument frame */
    q->p_taddr = 0;
    q->p_uaddr = 0;
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
 *
 * V6 sys1.c sbreak(), page-granular and separated-I&D (EXE) only.  The data
 * segment occupies the low window slots [0 .. p_dsize) and the stack the high
 * slots pinned at USTACK; break moves the data/stack boundary, sliding the
 * stack pages through core to keep them packed against the data (the window
 * inserts the unmapped heap gap between them).  u_arg[0] is the new break as a
 * byte offset from DS:0.
 */
void sbreak(void)
{
    register struct proc *p;
    register uint a;
    register int n;
    int nd, d, ns;

    p = u.u_procp;
    /*
     * nd = new data size in pages; d = new - old; ns = stack pages (break
     * does not touch the stack, only where it sits).
     */
    nd = (uint)u.u_arg[0] / PAGESIZ;
    if((uint)u.u_arg[0] % PAGESIZ)
        nd++;
    d = nd - p->p_dsize;
    ns = p->p_size - p->p_dsize;
    if(estabur(p->p_tsize, nd, ns, 1))      /* validate the new sizes (V6) */
        return;
    p->p_dsize = nd;
    if(d > 0)
        goto bigger;

    /*
     * Shrinking (d <= 0): slide the stack pages down into the freed data
     * space, then release the tail.
     */
    a = p->p_addr + nd;                     /* new stack base */
    n = ns;
    while(n--) {
        copyseg(a - d, a);                  /* d<=0: a-d = a+|d| is the old page */
        a++;
    }
    expand(nd + ns);                        /* shrink: frees the tail */
    estabur(p->p_tsize, nd, ns, 1);         /* install the new window map */
    return;

bigger:
    /*
     * Growing (d > 0): enlarge the block, slide the stack pages up, then clear
     * the d new data pages exposed below the stack.
     */
    expand(nd + ns);
    a = p->p_addr + p->p_size;              /* top of the new block */
    n = ns;
    while(n--) {
        a--;
        copyseg(a - d, a);
    }
    while(d--)
        clearseg(--a);
    estabur(p->p_tsize, nd, ns, 1);         /* install with the new p_dsize */
}
