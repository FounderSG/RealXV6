/*
 * Everything in this file is a routine implementing a system call.
 */

#include "os.h"

void getswit(void)
{
    u.u_ar0[R0] = getps();
}

void gtime(void)
{
    u.u_ar0[R0] = time[0];
    u.u_ar0[R1] = time[1];
}

void stime(void)
{
    if(suser()) {
        time[0] = u.u_ar0[R0];
        time[1] = u.u_ar0[R1];
        wakeup(tout);
    }
}

void setuid(void)
{
    char uid;

    uid = u.u_ar0[R0] & 0xff;
    if(u.u_ruid == uid || suser()) {
        u.u_uid = uid;
        u.u_procp->p_uid = uid;
        u.u_ruid = uid;
    }
}

void getuid(void)
{
    u.u_ar0[R0] = (u.u_uid << 8) | u.u_ruid;
}

void setgid(void)
{
    char gid;

    gid = u.u_ar0[R0] & 0xff;
    if(u.u_rgid == gid || suser()) {
        u.u_gid = gid;
        u.u_rgid = gid;
    }
}

void getgid(void)
{
    u.u_ar0[R0] = (u.u_gid<<8) + u.u_rgid;
}

void getpid(void)
{
    u.u_ar0[R0] = u.u_procp->p_pid;
}

void sync(void)
{
    update();
}

void nice(void)
{
    int n;

    n = u.u_ar0[R0];
    if(n > 20)
        n = 20;
    if(n < 0 && !suser())
        n = 0;
    u.u_procp->p_nice = n;
}

/*
 * Unlink system call.
 * panic: unlink -- "cannot happen"
 */
void unlink(void)
{
    struct inode *ip, *pp;

    pp = namei(&uchar, 2);
    if(pp == NULL)
        return;
    prele(pp);
    ip = iget(pp->i_dev, u.u_dent.u_ino);
    if(ip == NULL)
        panic("unlink -- iget");
    if((ip->i_mode&IFMT)==IFDIR && !suser())
        goto out;
    u.u_offset[1] -= DIRSIZ+2;
    u.u_base = (char *)&u.u_dent;
    u.u_count = DIRSIZ+2;
    u.u_dent.u_ino = 0;
    writei(pp);
    ip->i_nlink--;
    ip->i_flag |= IUPD;

out:
    iput(pp);
    iput(ip);
}

void chdir(void)
{
    struct inode *ip;

    ip = namei(&uchar, 0);
    if(ip == NULL)
        return;
    if((ip->i_mode&IFMT) != IFDIR) {
        u.u_error = ENOTDIR;
    bad:
        iput(ip);
        return;
    }
    if(access(ip, IEXEC))
        goto bad;
    iput(u.u_cdir);
    u.u_cdir = ip;
    prele(ip);
}

void chmod(void)
{
    struct inode *ip;

    if ((ip = owner()) == NULL)
        return;
    ip->i_mode &= ~07777;
    if (u.u_uid)
        u.u_arg[1] &= ~ISVTX;
    ip->i_mode |= u.u_arg[1]&07777;
    ip->i_flag |= IUPD;
    iput(ip);
}

void chown(void)
{
    struct inode *ip;

    if (!suser() || (ip = owner()) == NULL)
        return;
    ip->i_uid = u.u_arg[1] & 0xff;
    ip->i_gid = u.u_arg[1] >> 8;
    ip->i_flag |= IUPD;
    iput(ip);
}

/*
 * Change modified date of file:
 * time to r0-r1; sys smdate; file
 * This call has been withdrawn because it messes up
 * incremental dumps (pseudo-old files aren't dumped).
 * It works though and you can uncomment it if you like.

smdate()
{
    register struct inode *ip;
    register int *tp;
    int tbuf[2];

    if ((ip = owner()) == NULL)
        return;
    ip->i_flag =| IUPD;
    tp = &tbuf[2];
    *--tp = u.u_ar0[R1];
    *--tp = u.u_ar0[R0];
    iupdat(ip, tp);
    ip->i_flag =& ~IUPD;
    iput(ip);
}
*/

void ssig(void)
{
    int a;

    a = u.u_arg[0];
    if(a<=0 || a>=NSIG || a ==SIGKIL) {
        u.u_error = EINVAL;
        return;
    }
    u.u_ar0[R0] = u.u_signal[a];
    u.u_signal[a] = u.u_arg[1];
    if(u.u_procp->p_sig == a)
        u.u_procp->p_sig = 0;
}

void kill(void)
{
    register struct proc *p, *q;
    int a;
    int f;

    f = 0;
    a = u.u_ar0[R0];
    q = u.u_procp;
    for(p = &proc[0]; p < &proc[NPROC]; p++) {
        if(p == q)
            continue;
        if(a != 0 && p->p_pid != a)
            continue;
        if(a == 0 && (p->p_ttyp != q->p_ttyp || p <= &proc[1]))
            continue;
        if(u.u_uid != 0 && u.u_uid != p->p_uid)
            continue;
        f++;
        psignal(p, u.u_arg[0]);
    }
    if(f == 0)
        u.u_error = ESRCH;
}

void times(void)
{
    int *p;

    for(p = &u.u_utime; p  < &u.u_utime+6;) {
        suword(u.u_arg[0], *p++);
        u.u_arg[0] += 2;
    }
}

#if PDP11
profil()
{
    u.u_prof[0] = u.u_arg[0] & ~1;  /* base of sample buf */
    u.u_prof[1] = u.u_arg[1];   /* size of same */
    u.u_prof[2] = u.u_arg[2];   /* pc offset */
    u.u_prof[3] = (u.u_arg[3]>>1) & 077777; /* pc scale */
}
#endif

/*
 * psinfo - process status query (replaces the old getkaddr peek).
 *
 * Fill the caller's buffer with the struct below, followed by a 512-byte image
 * of the top of that process's user stack, where exec leaves the argument
 * vector.  ps assembles the COMMAND column from that image, so it never reads
 * /dev/mem or /dev/kmem and never needs to know the physical layout: the
 * kernel, which owns the mapping, resolves it here.  This is also what lets the
 * data segment be mapped sparsely -- ps no longer assumes a flat p_addr image.
 *
 * ps carries its own copy of the struct, extended with the trailing image, and
 * includes no kernel header at all; a new field belongs ahead of the image in
 * both.  Filling the fields one at a time keeps struct proc private to the
 * kernel.  Every scalar is int: p_pri is signed, and a char field would be
 * signed only while both builds carry Watcom's -j.  R0 returns 0; an index past
 * the end of the proc table is an EINVAL error, which is how ps finds the end.
 *
 * stkbase reports the user address the image came from, or 0 when no frame was
 * captured: a free slot, a process carrying p_tsize==0, or an unreadable swap
 * block.  p_tsize==0 means a zombie -- exit clears it along with the text and
 * leaves p_addr pointing at a one-block image of u, which holds no argument
 * frame -- or proc 0, which never exec'd (exec rejects an EXE with no text, so
 * every other live process has one).  ps consults stkbase before parsing, so a
 * stale image left in the caller's buffer by an earlier call is never mistaken
 * for this process's arguments.
 *
 * In core the copy is a non-blocking far memcpy, so it cannot race the swapper.
 * For a swapped-out process the stack block is read from the swap device, which
 * sleeps; the process may be swapped back in, or the slot reused, while we
 * sleep, leaving the block stale.  Snapshot {p_pid,p_addr,SLOAD} across the
 * bread and retry if it moved; a retry re-reads the slot from the top, so it
 * also copes with the slot now holding a different process, or none at all.
 */
struct psbuf
{
    int     p_stat;         /* 0 marks a free slot */
    int     p_flag;
    int     p_pri;          /* priority, negative is high */
    int     p_uid;
    int     p_pid;
    int     p_ppid;
    int     p_addr;
    int     p_wchan;
    int     stkbase;        /* user address the frame came from, 0 = none */
};

void psinfo(void)
{
    struct proc *p;
    struct buf *bp;
    struct psbuf pb;
    int idx, oaddr, opid, osize;
    uint udst, sdst;

    idx = u.u_ar0[R0];
    udst = (uint)u.u_arg[0];
    if(idx < 0 || idx >= NPROC) {
        u.u_error = EINVAL;
        return;
    }
    p = &proc[idx];
    sdst = udst + sizeof(pb);
    pb.stkbase = 0;                     /* no frame captured yet */

loop:
    if(p->p_stat == 0 || p->p_tsize == 0)
        goto out;                       /* free slot, or no EXE frame */
    oaddr = p->p_addr;
    osize = p->p_size;
    opid  = p->p_pid;
    /*
     * The user stack sits at the tail of the (possibly sparse) data block; exec
     * leaves the arg frame in the top 512-byte block of the top stack page,
     * which is the block's last physical page (p_addr + p_size - 1) at page
     * offset PAGESIZ-512 = 0xE00.  (USTACK's top 2 bytes are unused, so this
     * block is 512-aligned and a single swap read suffices.)  This reaches any
     * process (current or not) via the identity map, since core blocks are
     * allocated at physical pages >= USPACE, outside the windows.
     */
    if(p->p_flag & SLOAD) {
        /* In core: read the target's top page by identity map (no sleep, so it
         * cannot race the swapper).  Write into ps's own buffer through ps's
         * data window (user_dseg = WIN_DATA), not a raw p_addr identity offset:
         * the data block now starts at slot 1 (slot 0 = u), so a p_addr-based
         * offset would land one page low, in ps's u-area. */
        memcpy(MK_FP((unsigned)user_dseg, sdst),
               MK_FP((unsigned)(oaddr+osize-1)*(PAGESIZ/16), PAGESIZ-512),
               512);
    } else {
        bp = bread(swapdev, oaddr + (osize-1)*(PAGESIZ/512) + (PAGESIZ/512 - 1));
        if(p->p_pid != opid || (p->p_flag&SLOAD) || p->p_addr != oaddr) {
            brelse(bp);                 /* swapped in while we slept; reread */
            goto loop;
        }
        if(bp->b_flags & B_ERROR) {
            brelse(bp);
            goto out;                   /* unreadable: stkbase stays 0 */
        }
        if(copyout((uint)bp->b_addr, sdst, 512)) {
            brelse(bp);
            u.u_error = EFAULT;
            return;
        }
        brelse(bp);
    }
    pb.stkbase = (USTACK-2) & ~0x1FF;   /* 512-aligned base of the top block */

    /*
     * Read the scalars only now that the frame has settled.  bread sleeps,
     * and a retry can find the slot holding a different process, so a
     * snapshot taken before the read could describe one process while the
     * frame beside it came from another.  Nothing below sleeps, so the two
     * halves always describe the same instant.
     */
out:
    pb.p_stat = p->p_stat;
    pb.p_flag = p->p_flag & 0377;
    pb.p_pri = p->p_pri;
    pb.p_uid = p->p_uid & 0377;
    pb.p_pid = p->p_pid;
    pb.p_ppid = p->p_ppid;
    pb.p_addr = p->p_addr;
    pb.p_wchan = p->p_wchan;
    if(copyout((uint)&pb, udst, sizeof(pb))) {
        u.u_error = EFAULT;
        return;
    }
    u.u_ar0[R0] = 0;
}

/*
 * halt -- write the buffer cache out to the disk, wait for the disk to go
 * quiet, then stop the processor.  This is the stopunix() Peter Collinson
 * added to sys4.c in 1976 to support killunix, kept with his V6 sources at
 * https://github.com/pcollinson/unixv6-extras (halt/), and it is a system
 * call for a reason: sync(2) only *starts* the writes.  update() ends in
 * bflush(), which marks each delayed-write buffer B_ASYNC, hands it to the
 * driver and returns with the transfers still queued -- a program that called
 * sync() and then stopped the machine would leave behind exactly the
 * corruption a clean shutdown is meant to prevent.  So the flush and the stop
 * happen here, without going back to user mode in between.
 *
 * What to wait ON is the subtlety.  Not B_BUSY: a buffer is busy from
 * notavail() until whoever took it gives it back, and two of them are never
 * given back at all -- iinit() holds the root superblock in mount[0].m_bufp
 * and smount() holds one per mounted filesystem.  Waiting for "no buffer is
 * busy" hangs on the first of those.  B_ASYNC is the right flag: bflush()
 * sets it on exactly the buffers handed to the driver without waiting, and
 * brelse() clears it when iodone() gives one back, so B_ASYNC means "in
 * flight".  Reads and synchronous writes need no waiting -- whoever issued
 * them is already in iowait().  brelse() wakes us through B_WANTED.
 *
 * EBUSY when an update() is already running: updlock would make ours a no-op
 * (update() returns at once) and we would stop on a cache nobody flushed.
 * The caller retries; /bin/halt does.
 */
void halt(void)
{
    struct buf *bp;

    if(!suser())
        return;
    if(updlock) {
        u.u_error = EBUSY;
        return;
    }
    printf("halt: flushing\r\n");
    update();
    for(bp = &buf[0]; bp < &buf[NBUF]; bp++) {
again:
        spl6();
        if(bp->b_flags & B_ASYNC) {
            bp->b_flags |= B_WANTED;
            sleep(bp, PRIBIO);
            spl0();
            goto again;
        }
        spl0();
    }
    printf("safe to poweroff\r\n");
    stopit();
}
